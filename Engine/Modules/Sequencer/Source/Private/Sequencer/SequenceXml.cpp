#include "Public/Sequencer/SequenceFile.h"

#if WITH_UNCOOKED   // every build but Shipping: the editor, and a game run uncooked

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <pugixml.hpp>

/*
 * The authored .nmseq:
 *
 *   <Sequence version="1" fps="24" length="240">
 *     <Binding id="hale" kind="possess" guid="7f3a..." name="Inspector_Hale">
 *       <Track type="Transform">
 *         <Channel name="Position.X">
 *           <Key f="24" v="-4" interp="CubicInOut"/>
 *         </Channel>
 *       </Track>
 *       <Track type="Float" component="Camera" field="FovY">
 *         <Key f="0" v="55"/>
 *       </Track>
 *     </Binding>
 *     <Binding id="guard" kind="spawn" prefab="Prefabs/Guard.nmprefab"/>
 *     <Track type="Event">
 *       <Key f="228" event="Combat.Start" arg0="cellar" arg1="@hale" fireOnSkip="true"/>
 *     </Track>
 *   </Sequence>
 *
 * Section tracks hold <Section start="0" end="96" clip="..."/>: the asset
 * attribute is named for the track -- clip (Animation), sound (Audio),
 * camera (CameraCut: a binding id), sequence (SubSequence) -- with rate,
 * volume, offset, blendIn, blendOut and loop when not their defaults.
 * <Marker f="96" name="Reveal"/> names a frame. A track may carry name=""
 * and muted="true".
 *
 * A key's interp is omitted when it is Auto (a Bool track's always are), a
 * User key adds in="" and out="" (its tangents), an event's fireOnSkip is
 * omitted when false. Written deterministically -- two spaces of
 * indent, LF, attributes in a fixed order -- so a save that changes nothing
 * changes nothing in version control.
 */
namespace Sequencer
{
    namespace
    {
        constexpr int32_t FORMAT_VERSION = 1;

        std::string FormatFloat(const float InValue)
        {
            char Buffer[32];
            std::snprintf(Buffer, sizeof(Buffer), "%.9g", double(InValue));
            return Buffer;
        }

        bool ReadKeys(const pugi::xml_node& InParent, const bool bInBool, SequenceChannel& OutChannel, std::string& OutError)
        {
            for(pugi::xml_node Key = InParent.child("Key"); Key; Key = Key.next_sibling("Key"))
            {
                if(!Key.attribute("f"))
                {
                    OutError = "a <Key> has no frame (f)";
                    return false;
                }
                SequenceKey Made;
                Made.Frame = Key.attribute("f").as_int();
                Made.Value = Key.attribute("v").as_float();
                if(bInBool)
                {
                    /* v="true" / "false" reads too. */
                    const pugi::xml_attribute Value = Key.attribute("v");
                    Made.Value = (std::strcmp(Value.value(), "true") == 0 || Value.as_float() != 0.0f) ? 1.0f : 0.0f;
                    Made.Interp = ESequenceInterp::Constant;
                }
                else if(const pugi::xml_attribute Interp = Key.attribute("interp"))
                {
                    if(!ParseInterp(Interp.value(), Made.Interp))
                    {
                        OutError = std::string("interp=\"") + Interp.value() + "\" is not an interpolation";
                        return false;
                    }
                    Made.InTangent  = Key.attribute("in").as_float();
                    Made.OutTangent = Key.attribute("out").as_float();
                }
                OutChannel.Keys.push_back(Made);
            }
            return true;
        }

        /* The attribute a section names its asset by, on InType's track. */
        const char* SectionAssetAttribute(const ESequenceTrackType InType)
        {
            switch(InType)
            {
                case ESequenceTrackType::Animation:   return "clip";
                case ESequenceTrackType::Audio:       return "sound";
                case ESequenceTrackType::CameraCut:   return "camera";
                case ESequenceTrackType::SubSequence: return "sequence";
                default:                              return "asset";
            }
        }

        bool ReadTrack(const pugi::xml_node& InNode, SequenceTrack& OutTrack, std::string& OutError)
        {
            const char* TypeName = InNode.attribute("type").as_string();
            if(!ParseTrackType(TypeName, OutTrack.Type))
            {
                OutError = std::string("<Track type=\"") + TypeName + "\"> is not a track type";
                return false;
            }
            OutTrack.Component = InNode.attribute("component").as_string();
            OutTrack.Field     = InNode.attribute("field").as_string();
            OutTrack.Name      = InNode.attribute("name").as_string();
            OutTrack.bMuted    = InNode.attribute("muted").as_bool();
            OutTrack.Normalize();   // sizes Channels for the type

            switch(OutTrack.Type)
            {
                case ESequenceTrackType::Transform:
                    for(pugi::xml_node Channel = InNode.child("Channel"); Channel; Channel = Channel.next_sibling("Channel"))
                    {
                        const size_t Index = FindTransformChannel(Channel.attribute("name").as_string());
                        if(Index >= TC_Count)
                        {
                            OutError = std::string("<Channel name=\"") + Channel.attribute("name").as_string()
                                     + "\"> is not a Transform channel (Position.X ... Scale.Z)";
                            return false;
                        }
                        if(!ReadKeys(Channel, false, OutTrack.Channels[Index], OutError)) { return false; }
                    }
                    break;
                case ESequenceTrackType::Float:
                case ESequenceTrackType::Bool:
                case ESequenceTrackType::Fade:
                    if(!ReadKeys(InNode, OutTrack.Type == ESequenceTrackType::Bool, OutTrack.Channels[0], OutError)) { return false; }
                    break;
                case ESequenceTrackType::Animation:
                case ESequenceTrackType::Audio:
                case ESequenceTrackType::CameraCut:
                case ESequenceTrackType::SubSequence:
                    for(pugi::xml_node Node = InNode.child("Section"); Node; Node = Node.next_sibling("Section"))
                    {
                        SequenceSection Made;
                        Made.Start    = Node.attribute("start").as_int();
                        Made.End      = Node.attribute("end").as_int();
                        Made.Asset    = Node.attribute(SectionAssetAttribute(OutTrack.Type)).as_string(Node.attribute("asset").as_string());
                        Made.PlayRate = Node.attribute("rate").as_float(1.0f);
                        Made.Volume   = Node.attribute("volume").as_float(1.0f);
                        Made.Offset   = Node.attribute("offset").as_int(0);
                        Made.BlendIn  = Node.attribute("blendIn").as_int(0);
                        Made.BlendOut = Node.attribute("blendOut").as_int(0);
                        Made.bLoop    = Node.attribute("loop").as_bool();
                        OutTrack.Sections.push_back(std::move(Made));
                    }
                    break;
                case ESequenceTrackType::Event:
                    for(pugi::xml_node Key = InNode.child("Key"); Key; Key = Key.next_sibling("Key"))
                    {
                        SequenceEventKey Made;
                        Made.Frame       = Key.attribute("f").as_int();
                        Made.Event       = Key.attribute("event").as_string();
                        Made.bFireOnSkip = Key.attribute("fireOnSkip").as_bool();
                        for(size_t Arg = 0; Arg < SEQUENCE_EVENT_ARGS_MAX; ++Arg)
                        {
                            const std::string Name = "arg" + std::to_string(Arg);
                            const pugi::xml_attribute Value = Key.attribute(Name.c_str());
                            if(!Value) { break; }
                            Made.Args.push_back(SequenceEventArg::FromText(Value.value()));
                        }
                        OutTrack.Events.push_back(std::move(Made));
                    }
                    break;
                case ESequenceTrackType::Count:
                    break;
            }
            return true;
        }

        void WriteKeys(pugi::xml_node& InParent, const SequenceChannel& InChannel, const bool bInBool)
        {
            for(const SequenceKey& Key : InChannel.Keys)
            {
                pugi::xml_node Node = InParent.append_child("Key");
                Node.append_attribute("f").set_value(Key.Frame);
                Node.append_attribute("v").set_value(FormatFloat(Key.Value).c_str());
                if(!bInBool && Key.Interp != ESequenceInterp::Auto)
                {
                    Node.append_attribute("interp").set_value(InterpName(Key.Interp));
                }
                if(!bInBool && Key.Interp == ESequenceInterp::User)
                {
                    Node.append_attribute("in").set_value(FormatFloat(Key.InTangent).c_str());
                    Node.append_attribute("out").set_value(FormatFloat(Key.OutTangent).c_str());
                }
            }
        }

        void WriteTrack(pugi::xml_node& InParent, const SequenceTrack& InTrack)
        {
            pugi::xml_node Node = InParent.append_child("Track");
            Node.append_attribute("type").set_value(TrackTypeName(InTrack.Type));
            if(!InTrack.Name.empty()) { Node.append_attribute("name").set_value(InTrack.Name.c_str()); }
            if(InTrack.bMuted)        { Node.append_attribute("muted").set_value("true"); }
            switch(InTrack.Type)
            {
                case ESequenceTrackType::Transform:
                    for(size_t Index = 0; Index < InTrack.Channels.size() && Index < TC_Count; ++Index)
                    {
                        if(InTrack.Channels[Index].IsEmpty()) { continue; }
                        pugi::xml_node Channel = Node.append_child("Channel");
                        Channel.append_attribute("name").set_value(TransformChannelName(Index));
                        WriteKeys(Channel, InTrack.Channels[Index], false);
                    }
                    break;
                case ESequenceTrackType::Float:
                case ESequenceTrackType::Bool:
                    Node.append_attribute("component").set_value(InTrack.Component.c_str());
                    Node.append_attribute("field").set_value(InTrack.Field.c_str());
                    if(!InTrack.Channels.empty())
                    {
                        WriteKeys(Node, InTrack.Channels[0], InTrack.Type == ESequenceTrackType::Bool);
                    }
                    break;
                case ESequenceTrackType::Fade:
                    if(!InTrack.Channels.empty()) { WriteKeys(Node, InTrack.Channels[0], false); }
                    break;
                case ESequenceTrackType::Animation:
                case ESequenceTrackType::Audio:
                case ESequenceTrackType::CameraCut:
                case ESequenceTrackType::SubSequence:
                    for(const SequenceSection& Section : InTrack.Sections)
                    {
                        pugi::xml_node Made = Node.append_child("Section");
                        Made.append_attribute("start").set_value(Section.Start);
                        Made.append_attribute("end").set_value(Section.End);
                        Made.append_attribute(SectionAssetAttribute(InTrack.Type)).set_value(Section.Asset.c_str());
                        if(Section.PlayRate != 1.0f) { Made.append_attribute("rate").set_value(FormatFloat(Section.PlayRate).c_str()); }
                        if(Section.Volume != 1.0f)   { Made.append_attribute("volume").set_value(FormatFloat(Section.Volume).c_str()); }
                        if(Section.Offset != 0)      { Made.append_attribute("offset").set_value(Section.Offset); }
                        if(Section.BlendIn != 0)     { Made.append_attribute("blendIn").set_value(Section.BlendIn); }
                        if(Section.BlendOut != 0)    { Made.append_attribute("blendOut").set_value(Section.BlendOut); }
                        if(Section.bLoop)            { Made.append_attribute("loop").set_value("true"); }
                    }
                    break;
                case ESequenceTrackType::Event:
                    for(const SequenceEventKey& Event : InTrack.Events)
                    {
                        pugi::xml_node Key = Node.append_child("Key");
                        Key.append_attribute("f").set_value(Event.Frame);
                        Key.append_attribute("event").set_value(Event.Event.c_str());
                        for(size_t Arg = 0; Arg < Event.Args.size() && Arg < SEQUENCE_EVENT_ARGS_MAX; ++Arg)
                        {
                            const std::string Name = "arg" + std::to_string(Arg);
                            Key.append_attribute(Name.c_str()).set_value(Event.Args[Arg].ToText().c_str());
                        }
                        if(Event.bFireOnSkip) { Key.append_attribute("fireOnSkip").set_value("true"); }
                    }
                    break;
                case ESequenceTrackType::Count:
                    break;
            }
        }
    }

    bool ParseSequenceXml(const std::string& InText, SequenceAsset& OutAsset, std::string* OutError)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } return false; };

        pugi::xml_document Document;
        const pugi::xml_parse_result Parsed = Document.load_buffer(InText.data(), InText.size(), pugi::parse_default, pugi::encoding_utf8);
        if(!Parsed)
        {
            return Fail(std::string("XML at byte ") + std::to_string(Parsed.offset) + ": " + Parsed.description());
        }
        const pugi::xml_node Root = Document.child("Sequence");
        if(!Root) { return Fail("no <Sequence> root element"); }

        const int32_t Version = Root.attribute("version").as_int(1);
        if(Version > FORMAT_VERSION)
        {
            return Fail("version " + std::to_string(Version) + " is newer than this build reads");
        }

        SequenceAsset Asset;
        Asset.FrameRate = Root.attribute("fps").as_uint(24);
        Asset.Length    = Root.attribute("length").as_int(0);

        std::string Error;
        for(pugi::xml_node Child = Root.first_child(); Child; Child = Child.next_sibling())
        {
            if(Child.type() != pugi::node_element) { continue; }
            if(std::strcmp(Child.name(), "Binding") == 0)
            {
                SequenceBinding Binding;
                Binding.Id   = Child.attribute("id").as_string();
                Binding.Name = Child.attribute("name").as_string();
                const char* Kind = Child.attribute("kind").as_string("possess");
                if(std::strcmp(Kind, "possess") == 0)    { Binding.Kind = ESequenceBindingKind::Possess; }
                else if(std::strcmp(Kind, "spawn") == 0) { Binding.Kind = ESequenceBindingKind::Spawn; }
                else { return Fail(std::string("<Binding kind=\"") + Kind + "\"> is neither possess nor spawn"); }

                if(const pugi::xml_attribute Guid = Child.attribute("guid"))
                {
                    if(!GameCore::EntityGuid::Parse(Guid.value(), Binding.Guid))
                    {
                        return Fail("binding '" + Binding.Id + "': guid is not 32 hex digits");
                    }
                }
                Binding.Prefab = Child.attribute("prefab").as_string();
                for(pugi::xml_node TrackNode = Child.child("Track"); TrackNode; TrackNode = TrackNode.next_sibling("Track"))
                {
                    SequenceTrack Track;
                    if(!ReadTrack(TrackNode, Track, Error)) { return Fail("binding '" + Binding.Id + "': " + Error); }
                    Binding.Tracks.push_back(std::move(Track));
                }
                Asset.Bindings.push_back(std::move(Binding));
            }
            else if(std::strcmp(Child.name(), "Track") == 0)
            {
                SequenceTrack Track;
                if(!ReadTrack(Child, Track, Error)) { return Fail(Error); }
                Asset.Tracks.push_back(std::move(Track));
            }
            else if(std::strcmp(Child.name(), "Marker") == 0)
            {
                SequenceAsset::Marker Marker;
                Marker.Frame = Child.attribute("f").as_int();
                Marker.Name  = Child.attribute("name").as_string();
                Asset.Markers.push_back(std::move(Marker));
            }
            else
            {
                return Fail(std::string("<") + Child.name() + "> does not belong in a <Sequence>");
            }
        }
        Asset.Normalize();
        OutAsset = std::move(Asset);
        return true;
    }

    std::string WriteSequenceXml(const SequenceAsset& InAsset)
    {
        pugi::xml_document Document;
        pugi::xml_node Root = Document.append_child("Sequence");
        Root.append_attribute("version").set_value(FORMAT_VERSION);
        Root.append_attribute("fps").set_value(InAsset.FrameRate);
        Root.append_attribute("length").set_value(InAsset.Length);

        for(const SequenceBinding& Binding : InAsset.Bindings)
        {
            pugi::xml_node Node = Root.append_child("Binding");
            Node.append_attribute("id").set_value(Binding.Id.c_str());
            Node.append_attribute("kind").set_value(Binding.Kind == ESequenceBindingKind::Spawn ? "spawn" : "possess");
            if(Binding.Guid.IsSet())
            {
                char Text[GameCore::EntityGuid::STRING_BUFFER];
                Binding.Guid.ToString(Text);
                Node.append_attribute("guid").set_value(Text);
            }
            if(!Binding.Name.empty())   { Node.append_attribute("name").set_value(Binding.Name.c_str()); }
            if(!Binding.Prefab.empty()) { Node.append_attribute("prefab").set_value(Binding.Prefab.c_str()); }
            for(const SequenceTrack& Track : Binding.Tracks) { WriteTrack(Node, Track); }
        }
        for(const SequenceTrack& Track : InAsset.Tracks) { WriteTrack(Root, Track); }
        for(const SequenceAsset::Marker& Marker : InAsset.Markers)
        {
            pugi::xml_node Node = Root.append_child("Marker");
            Node.append_attribute("f").set_value(Marker.Frame);
            Node.append_attribute("name").set_value(Marker.Name.c_str());
        }

        std::ostringstream Stream;
        Document.save(Stream, "  ", pugi::format_indent, pugi::encoding_utf8);
        return Stream.str();
    }

    bool SaveSequenceXml(const std::string& InDiskPath, const SequenceAsset& InAsset, std::string* OutError)
    {
        const std::string Text = WriteSequenceXml(InAsset);
        const std::filesystem::path Final = std::filesystem::u8path(InDiskPath);
        const std::filesystem::path Temp  = std::filesystem::u8path(InDiskPath + ".tmp");
        {
            std::ofstream File(Temp, std::ios::binary | std::ios::trunc);
            if(!File || !File.write(Text.data(), std::streamsize(Text.size())))
            {
                if(OutError) { *OutError = "cannot write " + Temp.u8string(); }
                return false;
            }
        }
        std::error_code Error;
        std::filesystem::rename(Temp, Final, Error);
        if(Error)
        {
            std::filesystem::remove(Temp, Error);
            if(OutError) { *OutError = "cannot replace " + InDiskPath; }
            return false;
        }
        return true;
    }

    bool CookSequenceFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError)
    {
        std::ifstream In(std::filesystem::u8path(InSourceDiskPath), std::ios::binary);
        if(!In)
        {
            if(OutError) { *OutError = "cannot read " + InSourceDiskPath; }
            return false;
        }
        const std::vector<uint8_t> Source((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

        /* Either form: a sequence already cooked cooks to itself. */
        SequenceAsset Asset;
        if(!ReadSequence(Source, Asset, OutError)) { return false; }

        const std::vector<uint8_t> Bytes = WriteSequenceBinary(Asset);
        std::error_code Error;
        const std::filesystem::path Out = std::filesystem::u8path(InOutDiskPath);
        if(Out.has_parent_path()) { std::filesystem::create_directories(Out.parent_path(), Error); }
        std::ofstream File(Out, std::ios::binary | std::ios::trunc);
        if(!File || !File.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size())))
        {
            if(OutError) { *OutError = "cannot write " + InOutDiskPath; }
            return false;
        }
        return true;
    }
}

#endif // WITH_UNCOOKED
