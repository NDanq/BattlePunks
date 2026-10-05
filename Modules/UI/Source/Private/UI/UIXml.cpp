#include "Public/UI/UIFile.h"

#if WITH_EDITOR

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <pugixml.hpp>

#include "Public/UI/UIProperty.h"

/*
 * The authored .nmui: one <UI> root holding <Styles>, exactly one root widget
 * element, and the reserved <Animations> and <VisualStates>. A widget element
 * is its type's XML name; its attributes are its properties, as text, in the
 * order written; its child elements are its children.
 *
 * Written deterministically: the same asset always gives the same bytes --
 * two spaces of indent, LF, attributes in the order held -- so a save that
 * changes nothing changes nothing in version control.
 */
namespace UI
{
    namespace
    {
        bool IsReservedElement(const char* InName)
        {
            return std::strcmp(InName, "Animations") == 0 || std::strcmp(InName, "VisualStates") == 0;
        }

        std::string NodeText(const pugi::xml_node& InNode)
        {
            std::ostringstream Stream;
            InNode.print(Stream, "  ", pugi::format_indent | pugi::format_no_declaration, pugi::encoding_utf8);
            std::string Text = Stream.str();
            while(!Text.empty() && (Text.back() == '\n' || Text.back() == '\r')) { Text.pop_back(); }
            return Text;
        }

        /* <Animations> into Animations, where it reads; the text stays in Reserved either way. */
        void ReadAnimations(const pugi::xml_node& InNode, std::vector<UIAnimationAsset>& OutAnimations)
        {
            for(pugi::xml_node Animation = InNode.child("Animation"); Animation; Animation = Animation.next_sibling("Animation"))
            {
                UIAnimationAsset Made;
                Made.Name     = Animation.attribute("Name").as_string();
                Made.Duration = Animation.attribute("Duration").as_float();
                Made.bLoop    = Animation.attribute("Loop").as_bool();
                for(pugi::xml_node Track = Animation.child("Track"); Track; Track = Track.next_sibling("Track"))
                {
                    UIAnimationTrack T;
                    T.Target   = Track.attribute("Target").as_string();
                    T.Property = Track.attribute("Property").as_string();
                    T.Easing   = ParseEasing(Track.attribute("Easing").as_string("Linear"));
                    for(pugi::xml_node Key = Track.child("Key"); Key; Key = Key.next_sibling("Key"))
                    {
                        T.Keys.push_back({ Key.attribute("Time").as_float(), Key.attribute("Value").as_string() });
                    }
                    Made.Tracks.push_back(std::move(T));
                }
                OutAnimations.push_back(std::move(Made));
            }
        }

        /* InElement and its subtree onto OutWidgets, in document order. */
        bool ReadWidget(const pugi::xml_node& InElement, const int32_t InParent, std::vector<UIWidgetDesc>& OutWidgets, std::string& OutError)
        {
            UIWidgetDesc Desc;
            Desc.Type   = InElement.name();
            Desc.Parent = InParent;
            for(pugi::xml_attribute Attribute = InElement.first_attribute(); Attribute; Attribute = Attribute.next_attribute())
            {
                Desc.Properties.push_back({ Attribute.name(), Attribute.value() });
            }
            const int32_t Index = int32_t(OutWidgets.size());
            OutWidgets.push_back(std::move(Desc));
            for(pugi::xml_node Child = InElement.first_child(); Child; Child = Child.next_sibling())
            {
                if(Child.type() == pugi::node_comment) continue;
                if(Child.type() != pugi::node_element)
                {
                    OutError = "<" + std::string(InElement.name()) + "> has text inside it; only widget elements go there";
                    return false;
                }
                if(!ReadWidget(Child, Index, OutWidgets, OutError)) return false;
            }
            return true;
        }

        void WriteWidget(pugi::xml_node& InParent, const UIDocumentAsset& InAsset, const int32_t InIndex)
        {
            const UIWidgetDesc& Desc = InAsset.Widgets[size_t(InIndex)];
            pugi::xml_node Element = InParent.append_child(Desc.Type.c_str());
            for(const UIPropertyText& Each : Desc.Properties)
            {
                Element.append_attribute(Each.Name.c_str()).set_value(Each.Value.c_str());
            }
            for(const int32_t Child : InAsset.GetChildren(InIndex)) { WriteWidget(Element, InAsset, Child); }
        }

        std::string Save(const pugi::xml_document& InDocument)
        {
            std::ostringstream Stream;
            InDocument.save(Stream, "  ", pugi::format_indent, pugi::encoding_utf8);
            return Stream.str();
        }
    }

    bool ParseUIXml(const std::string& InText, UIDocumentAsset& OutAsset, std::string* OutError)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) *OutError = InWhy; return false; };

        pugi::xml_document Document;
        const pugi::xml_parse_result Parsed = Document.load_buffer(InText.data(), InText.size(),
                                                                   pugi::parse_default | pugi::parse_comments, pugi::encoding_utf8);
        if(!Parsed)
        {
            return Fail(std::string("XML at byte ") + std::to_string(Parsed.offset) + ": " + Parsed.description());
        }
        const pugi::xml_node Root = Document.child("UI");
        if(!Root) return Fail("no <UI> root element");

        UIDocumentAsset Asset;
        for(pugi::xml_attribute Attribute = Root.first_attribute(); Attribute; Attribute = Attribute.next_attribute())
        {
            if(!Asset.Settings.SetFromText(Attribute.name(), Attribute.value()))
            {
                return Fail(std::string("<UI ") + Attribute.name() + "=\"" + Attribute.value() + "\">: not a document setting, or a bad value");
            }
        }
        if(Asset.Settings.Format > UIDocumentAsset::FORMAT)
        {
            return Fail("format " + std::to_string(Asset.Settings.Format) + " is newer than this build reads");
        }

        std::string Error;
        for(pugi::xml_node Child = Root.first_child(); Child; Child = Child.next_sibling())
        {
            if(Child.type() == pugi::node_comment) continue;
            if(Child.type() != pugi::node_element) return Fail("<UI> has text inside it");

            const char* Name = Child.name();
            if(std::strcmp(Name, "Styles") == 0)
            {
                for(pugi::xml_node Style = Child.child("Style"); Style; Style = Style.next_sibling("Style"))
                {
                    UIStyle Made;
                    for(pugi::xml_attribute Attribute = Style.first_attribute(); Attribute; Attribute = Attribute.next_attribute())
                    {
                        if(std::strcmp(Attribute.name(), "Name") == 0)      Made.Name = Attribute.value();
                        else if(std::strcmp(Attribute.name(), "Type") == 0) Made.Type = Attribute.value();
                        else Made.Properties.push_back({ Attribute.name(), Attribute.value() });
                    }
                    if(Made.Name.empty()) return Fail("a <Style> has no Name");
                    Asset.Styles.push_back(std::move(Made));
                }
            }
            else if(IsReservedElement(Name))
            {
                Asset.Reserved.push_back(NodeText(Child));
                if(std::strcmp(Name, "Animations") == 0) { ReadAnimations(Child, Asset.Animations); }
            }
            else
            {
                if(!Asset.Widgets.empty()) return Fail("<UI> holds one root widget; <" + std::string(Name) + "> is a second");
                if(!ReadWidget(Child, -1, Asset.Widgets, Error)) return Fail(Error);
            }
        }
        if(Asset.Widgets.empty()) return Fail("<UI> holds no root widget");

        OutAsset = std::move(Asset);
        return true;
    }

    std::string WriteUIXml(const UIDocumentAsset& InAsset)
    {
        pugi::xml_document Document;
        pugi::xml_node Declaration = Document.append_child(pugi::node_declaration);
        Declaration.append_attribute("version").set_value("1.0");
        Declaration.append_attribute("encoding").set_value("utf-8");

        pugi::xml_node Root = Document.append_child("UI");
        for(const UIPropertyText& Each : InAsset.Settings.ToText())
        {
            Root.append_attribute(Each.Name.c_str()).set_value(Each.Value.c_str());
        }
        if(!InAsset.Styles.empty())
        {
            pugi::xml_node Styles = Root.append_child("Styles");
            for(const UIStyle& Style : InAsset.Styles)
            {
                pugi::xml_node Element = Styles.append_child("Style");
                Element.append_attribute("Name").set_value(Style.Name.c_str());
                if(!Style.Type.empty()) Element.append_attribute("Type").set_value(Style.Type.c_str());
                for(const UIPropertyText& Each : Style.Properties)
                {
                    Element.append_attribute(Each.Name.c_str()).set_value(Each.Value.c_str());
                }
            }
        }
        if(!InAsset.Widgets.empty()) { WriteWidget(Root, InAsset, 0); }
        for(const std::string& Each : InAsset.Reserved)
        {
            pugi::xml_document Section;
            if(Section.load_buffer(Each.data(), Each.size(), pugi::parse_default | pugi::parse_comments, pugi::encoding_utf8))
            {
                for(pugi::xml_node Node = Section.first_child(); Node; Node = Node.next_sibling()) { Root.append_copy(Node); }
            }
        }
        return Save(Document);
    }

    bool SaveUIXml(const std::string& InDiskPath, const UIDocumentAsset& InAsset, std::string* OutError)
    {
        const std::string Text = WriteUIXml(InAsset);
        const std::filesystem::path Final = std::filesystem::u8path(InDiskPath);
        const std::filesystem::path Temp  = std::filesystem::u8path(InDiskPath + ".tmp");
        {
            std::ofstream File(Temp, std::ios::binary | std::ios::trunc);
            if(!File || !File.write(Text.data(), std::streamsize(Text.size())))
            {
                if(OutError) *OutError = "cannot write " + Temp.u8string();
                return false;
            }
        }
        std::error_code Error;
        std::filesystem::rename(Temp, Final, Error);
        if(Error)
        {
            std::filesystem::remove(Temp, Error);
            if(OutError) *OutError = "cannot replace " + InDiskPath;
            return false;
        }
        return true;
    }

    bool CookUIFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError)
    {
        std::ifstream In(std::filesystem::u8path(InSourceDiskPath), std::ios::binary);
        if(!In)
        {
            if(OutError) *OutError = "cannot read " + InSourceDiskPath;
            return false;
        }
        const std::string Text((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());

        UIDocumentAsset Asset;
        if(!ParseUIXml(Text, Asset, OutError)) return false;
        std::string Invalid;
        UIDocumentAsset Applied = Asset;
        Applied.ApplyStyles();
        if(!Applied.Validate(&Invalid))
        {
            if(OutError) *OutError = Invalid;
            return false;
        }

        const std::vector<uint8_t> Bytes = WriteUIBinary(Asset);
        std::error_code Error;
        const std::filesystem::path Out = std::filesystem::u8path(InOutDiskPath);
        if(Out.has_parent_path()) std::filesystem::create_directories(Out.parent_path(), Error);
        std::ofstream File(Out, std::ios::binary | std::ios::trunc);
        if(!File || !File.write(reinterpret_cast<const char*>(Bytes.data()), std::streamsize(Bytes.size())))
        {
            if(OutError) *OutError = "cannot write " + InOutDiskPath;
            return false;
        }
        return true;
    }

    std::string WriteUIXmlFragment(const UIDocumentAsset& InAsset, const int32_t InIndex)
    {
        if(InIndex < 0 || size_t(InIndex) >= InAsset.Widgets.size()) return std::string();
        pugi::xml_document Document;
        pugi::xml_node Holder = Document.append_child("Fragment");
        WriteWidget(Holder, InAsset, InIndex);
        return NodeText(Holder.first_child());
    }

    bool ParseUIXmlFragment(const std::string& InText, std::vector<UIWidgetDesc>& OutWidgets, std::string* OutError)
    {
        pugi::xml_document Document;
        if(!Document.load_buffer(InText.data(), InText.size(), pugi::parse_default, pugi::encoding_utf8) || !Document.document_element())
        {
            if(OutError) *OutError = "not a widget's XML";
            return false;
        }
        std::vector<UIWidgetDesc> Widgets;
        std::string Error;
        if(!ReadWidget(Document.document_element(), -1, Widgets, Error))
        {
            if(OutError) *OutError = Error;
            return false;
        }
        if(!FindWidgetType(Widgets.front().Type.c_str()))
        {
            if(OutError) *OutError = "<" + Widgets.front().Type + "> is not a widget";
            return false;
        }
        OutWidgets = std::move(Widgets);
        return true;
    }
}

#endif // WITH_EDITOR
