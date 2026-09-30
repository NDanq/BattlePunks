#include "Public/NodeGraph/GraphFile.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>

#include "pugixml.hpp"

#include "Public/Core/CorePlatform.h"
#include "Public/NodeGraph/GraphCompiler.h"
#include "Public/NodeGraph/Schema.h"
#include "Private/NodeGraph/GraphIndex.h"
#include "Private/NodeGraph/NodeGraphLog.h"

namespace NodeGraph
{
namespace
{
    constexpr const char* TEMP_SUFFIX   = ".tmp";
    constexpr const char* ROOT_ELEMENT  = "NodeGraph";
    constexpr const char* FRAGMENT_ROOT = "NodeGraphFragment";

    std::string Lower(std::string InText)
    {
        std::transform(InText.begin(), InText.end(), InText.begin(), [](const unsigned char Ch) { return char(std::tolower(Ch)); });
        return InText;
    }

    size_t FileNameStart(const std::string& InPath)
    {
        const size_t Slash = InPath.find_last_of("/\\");
        return Slash == std::string::npos ? 0 : Slash + 1;
    }

    // === Files ==================================================================
    // Core's appFile* for reading and writing (UTF-8 paths); std::filesystem
    // only for what Core has no call for, a replacing rename -- as WorldPackage.

    bool ReadWholeFile(const std::string& InPath, std::string& OutText)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InPath, Bytes)) { return false; }
        OutText.assign(reinterpret_cast<const char*>(Bytes.data()), Bytes.size());
        return true;
    }

    bool WriteWholeFile(const std::string& InPath, const std::string& InText)
    {
        EngineCore::FileHandle Handle = EngineCore::appFileOpenWrite(InPath);
        if(!Handle) { return false; }
        const size_t Written = InText.empty() ? 0 : EngineCore::appFileWrite(Handle, InText.data(), InText.size());
        EngineCore::appFileClose(Handle);
        return Written == InText.size();
    }

    bool RenameReplacing(const std::string& InFrom, const std::string& InTo, std::string& OutError)
    {
        std::error_code Error;
        std::filesystem::rename(std::filesystem::u8path(InFrom), std::filesystem::u8path(InTo), Error);
        if(Error)
        {
            OutError = Error.message();
            return false;
        }
        return true;
    }

    // === XML ====================================================================

    struct StringWriter final : pugi::xml_writer
    {
        std::string Text;
        void write(const void* InData, const size_t InSize) override { Text.append(static_cast<const char*>(InData), InSize); }
    };

    /* Two-space indent, '\n' line endings, UTF-8 -- fixed, so the bytes depend
     * on the data alone. */
    std::string DocumentText(const pugi::xml_document& InDoc)
    {
        StringWriter Writer;
        InDoc.save(Writer, "  ", pugi::format_indent, pugi::encoding_utf8);
        return Writer.Text;
    }

    std::string ElementText(const pugi::xml_node InNode)
    {
        StringWriter Writer;
        InNode.print(Writer, "", pugi::format_raw, pugi::encoding_utf8);
        return Writer.Text;
    }

    std::string PinRefText(const PinRef& InRef)
    {
        return GuidToString(InRef.Node) + "." + InRef.Pin;
    }

    /* "<32 hex>.<pin>" -- split at the fixed GUID length, so a pin name may
     * itself contain a '.'. */
    bool ParsePinRef(const char* InText, PinRef& OutRef)
    {
        const std::string Text = InText ? InText : "";
        if(Text.size() < NodeGuid::STRING_LENGTH + 2 || Text[NodeGuid::STRING_LENGTH] != '.') { return false; }
        if(!GuidFromString(Text.substr(0, NodeGuid::STRING_LENGTH), OutRef.Node)) { return false; }
        OutRef.Pin = Text.substr(NodeGuid::STRING_LENGTH + 1);
        return true;
    }

    // --- Writing -----------------------------------------------------------------

    void WriteNode(pugi::xml_node InParent, const Node& InNode)
    {
        if(InNode.IsPlaceholder())
        {
            /* Verbatim, except where the editor may have moved it. */
            pugi::xml_document Kept;
            if(Kept.load_string(InNode.PlaceholderXml.c_str()) && Kept.first_child())
            {
                pugi::xml_node Copy = InParent.append_copy(Kept.first_child());
                for(const char* Axis : { "X", "Y" })
                {
                    pugi::xml_attribute Attribute = Copy.attribute(Axis);
                    if(!Attribute) { Attribute = Copy.append_attribute(Axis); }
                    Attribute.set_value(FormatNumber(Axis[0] == 'X' ? InNode.X : InNode.Y).c_str());
                }
                return;
            }
        }

        pugi::xml_node Element = InParent.append_child("Node");
        Element.append_attribute("Id")      = GuidToString(InNode.Id).c_str();
        Element.append_attribute("Type")    = InNode.Type.c_str();
        Element.append_attribute("Version") = InNode.Version;
        Element.append_attribute("X")       = FormatNumber(InNode.X).c_str();
        Element.append_attribute("Y")       = FormatNumber(InNode.Y).c_str();
        if(!InNode.WildcardType.empty())
        {
            Element.append_attribute("Wildcard") = InNode.WildcardType.c_str();
        }

        for(const auto& Constant : InNode.Constants)
        {
            pugi::xml_node Item = Element.append_child("Const");
            Item.append_attribute("Pin")   = Constant.first.c_str();
            Item.append_attribute("Value") = Constant.second.c_str();
        }
        for(const auto& Property : InNode.Properties)
        {
            pugi::xml_node Item = Element.append_child("Prop");
            Item.append_attribute("Name")  = Property.first.c_str();
            Item.append_attribute("Value") = Property.second.c_str();
        }
    }

    void WriteNodesAndLinks(pugi::xml_node InRoot, std::vector<const Node*> InNodes, std::vector<Link> InLinks)
    {
        std::stable_sort(InNodes.begin(), InNodes.end(), [](const Node* L, const Node* R) { return L->Id < R->Id; });
        std::sort(InLinks.begin(), InLinks.end());

        if(!InNodes.empty())
        {
            pugi::xml_node NodesElement = InRoot.append_child("Nodes");
            for(const Node* Each : InNodes) { WriteNode(NodesElement, *Each); }
        }
        if(!InLinks.empty())
        {
            pugi::xml_node LinksElement = InRoot.append_child("Links");
            for(const Link& Each : InLinks)
            {
                pugi::xml_node Item = LinksElement.append_child("Link");
                Item.append_attribute("From") = PinRefText(Each.From).c_str();
                Item.append_attribute("To")   = PinRefText(Each.To).c_str();
            }
        }
    }

    // --- Reading -----------------------------------------------------------------

    float ReadFloat(const pugi::xml_node InNode, const char* InName)
    {
        double Value = 0.0;
        return ParseNumber(InNode.attribute(InName).as_string(), Value) ? float(Value) : 0.0f;
    }

    bool ReadNode(const pugi::xml_node InElement, const Schema& InSchema, Node& OutNode, std::vector<std::string>& OutWarnings)
    {
        if(!GuidFromString(InElement.attribute("Id").as_string(), OutNode.Id))
        {
            OutWarnings.push_back(std::string("a node with a bad Id ('") + InElement.attribute("Id").as_string() + "') was dropped");
            return false;
        }

        OutNode.Type    = InElement.attribute("Type").as_string();
        OutNode.Version = InElement.attribute("Version").as_uint(1);
        OutNode.X       = ReadFloat(InElement, "X");
        OutNode.Y       = ReadFloat(InElement, "Y");
        OutNode.WildcardType = InElement.attribute("Wildcard").as_string();

        for(const pugi::xml_node Item : InElement.children("Const"))
        {
            OutNode.Constants[Item.attribute("Pin").as_string()] = Item.attribute("Value").as_string();
        }
        for(const pugi::xml_node Item : InElement.children("Prop"))
        {
            OutNode.Properties[Item.attribute("Name").as_string()] = Item.attribute("Value").as_string();
        }

        if(!InSchema.FindNode(OutNode.Type))
        {
            OutNode.PlaceholderXml = ElementText(InElement);
            OutWarnings.push_back("node " + ShortGuid(OutNode.Id) + " has the unknown type '" + OutNode.Type
                                  + "' and was kept as a placeholder");
        }
        return true;
    }

    /* The <Nodes> and <Links> of a graph or fragment. Links to nodes that are
     * not there are dropped with a warning; links to pins a node no longer has
     * are KEPT -- ValidateGraph flags them, and the user decides. */
    void ReadNodesAndLinks(const pugi::xml_node InRoot, const Schema& InSchema, Graph& OutGraph, std::vector<std::string>& OutWarnings)
    {
        std::set<NodeGuid> Ids;
        for(const pugi::xml_node Element : InRoot.child("Nodes").children("Node"))
        {
            Node Loaded;
            if(!ReadNode(Element, InSchema, Loaded, OutWarnings)) { continue; }
            if(!Ids.insert(Loaded.Id).second)
            {
                OutWarnings.push_back("a second node with the id " + GuidToString(Loaded.Id) + " was dropped");
                continue;
            }
            OutGraph.Nodes.push_back(std::move(Loaded));
        }

        for(const pugi::xml_node Element : InRoot.child("Links").children("Link"))
        {
            Link Loaded;
            if(!ParsePinRef(Element.attribute("From").as_string(), Loaded.From)
               || !ParsePinRef(Element.attribute("To").as_string(), Loaded.To))
            {
                OutWarnings.push_back(std::string("a malformed link ('") + Element.attribute("From").as_string() + "' -> '"
                                      + Element.attribute("To").as_string() + "') was dropped");
                continue;
            }
            if(!Ids.count(Loaded.From.Node) || !Ids.count(Loaded.To.Node))
            {
                OutWarnings.push_back("a link to a missing node (" + PinRefText(Loaded.From) + " -> " + PinRefText(Loaded.To) + ") was dropped");
                continue;
            }
            if(OutGraph.HasLink(Loaded)) { continue; }
            OutGraph.Links.push_back(std::move(Loaded));
        }
    }
}

// === Paths ======================================================================

std::string GetGraphFileTag(const std::string& InPath)
{
    const std::string Name = Lower(InPath.substr(FileNameStart(InPath)));
    const size_t ExtensionLength = std::strlen(GRAPH_EXTENSION);
    if(Name.size() <= ExtensionLength || Name.compare(Name.size() - ExtensionLength, ExtensionLength, GRAPH_EXTENSION) != 0)
    {
        return std::string();
    }

    const std::string Stem = Name.substr(0, Name.size() - ExtensionLength);   // "glow.material"
    const size_t Dot = Stem.find_last_of('.');
    if(Dot == std::string::npos || Dot == 0) { return std::string(); }
    return Stem.substr(Dot + 1);
}

const Schema* FindSchemaForPath(const std::string& InPath)
{
    const std::string Tag = GetGraphFileTag(InPath);
    return Tag.empty() ? nullptr : FindSchemaByFileTag(Tag);
}

std::string StripGraphExtension(const std::string& InPath)
{
    const size_t NameStart = FileNameStart(InPath);
    const std::string Name = Lower(InPath.substr(NameStart));
    const size_t ExtensionLength = std::strlen(GRAPH_EXTENSION);
    if(Name.size() <= ExtensionLength || Name.compare(Name.size() - ExtensionLength, ExtensionLength, GRAPH_EXTENSION) != 0)
    {
        return InPath;
    }

    std::string Base = InPath.substr(0, InPath.size() - ExtensionLength);
    /* Only a tag some schema owns is part of the extension: "Level.1.nmgraph"
     * keeps its ".1". */
    const std::string Tag = GetGraphFileTag(InPath);
    if(!Tag.empty() && FindSchemaByFileTag(Tag))
    {
        Base.resize(Base.size() - Tag.size() - 1);
    }
    return Base;
}

std::string MakeGraphPath(const std::string& InBasePath, const Schema& InSchema)
{
    return InBasePath + "." + InSchema.GetFileTag() + GRAPH_EXTENSION;
}

std::string GetGraphOutputPath(const std::string& InGraphPath, const Schema& InSchema)
{
    return StripGraphExtension(InGraphPath) + InSchema.GetOutputExtension();
}

std::string GetGraphBaseName(const std::string& InPath)
{
    const std::string Base = StripGraphExtension(InPath);
    return Base.substr(FileNameStart(Base));
}

// === Text =======================================================================

std::string WriteGraphText(const Graph& InGraph)
{
    pugi::xml_document Doc;
    pugi::xml_node Declaration = Doc.append_child(pugi::node_declaration);
    Declaration.append_attribute("version")  = "1.0";
    Declaration.append_attribute("encoding") = "UTF-8";

    pugi::xml_node Root = Doc.append_child(ROOT_ELEMENT);
    Root.append_attribute("Version") = GRAPH_FILE_FORMAT;
    Root.append_attribute("Schema")  = InGraph.SchemaName.c_str();

    if(!InGraph.Properties.empty())
    {
        pugi::xml_node Element = Root.append_child("Properties");
        for(const auto& Property : InGraph.Properties)
        {
            pugi::xml_node Item = Element.append_child("Property");
            Item.append_attribute("Name")  = Property.first.c_str();
            Item.append_attribute("Value") = Property.second.c_str();
        }
    }

    /* Variables keep the user's order -- it is the order the editor lists them. */
    if(!InGraph.Variables.empty())
    {
        pugi::xml_node Element = Root.append_child("Variables");
        for(const Variable& Each : InGraph.Variables)
        {
            pugi::xml_node Item = Element.append_child("Variable");
            Item.append_attribute("Name")    = Each.Name.c_str();
            Item.append_attribute("Type")    = Each.Type.c_str();
            Item.append_attribute("Default") = Each.Default.c_str();
        }
    }

    std::vector<const Node*> Nodes;
    for(const Node& Each : InGraph.Nodes) { Nodes.push_back(&Each); }
    WriteNodesAndLinks(Root, Nodes, InGraph.Links);

    if(!InGraph.Comments.empty())
    {
        pugi::xml_node Element = Root.append_child("Comments");
        for(const Comment& Each : InGraph.Comments)
        {
            pugi::xml_node Item = Element.append_child("Comment");
            Item.append_attribute("X")      = FormatNumber(Each.X).c_str();
            Item.append_attribute("Y")      = FormatNumber(Each.Y).c_str();
            Item.append_attribute("Width")  = FormatNumber(Each.Width).c_str();
            Item.append_attribute("Height") = FormatNumber(Each.Height).c_str();
            Item.append_attribute("Text")   = Each.Text.c_str();
        }
    }

    return DocumentText(Doc);
}

bool ReadGraphText(const std::string& InText, Graph& OutGraph, std::string& OutError, std::vector<std::string>* OutWarnings)
{
    std::vector<std::string> Warnings;

    pugi::xml_document Doc;
    const pugi::xml_parse_result Parsed = Doc.load_buffer(InText.data(), InText.size(), pugi::parse_default, pugi::encoding_utf8);
    if(!Parsed)
    {
        OutError = std::string("not valid XML: ") + Parsed.description() + " (at byte " + std::to_string(Parsed.offset) + ")";
        return false;
    }

    const pugi::xml_node Root = Doc.child(ROOT_ELEMENT);
    if(!Root)
    {
        OutError = std::string("not a node graph (no <") + ROOT_ELEMENT + "> element)";
        return false;
    }

    const int Format = Root.attribute("Version").as_int(0);
    if(Format < 1 || Format > GRAPH_FILE_FORMAT)
    {
        OutError = "unsupported graph file version " + std::to_string(Format) + " (this editor reads up to "
                 + std::to_string(GRAPH_FILE_FORMAT) + ")";
        return false;
    }

    Graph Result;
    Result.SchemaName = Root.attribute("Schema").as_string();
    const Schema* S = FindSchema(Result.SchemaName);
    if(!S)
    {
        OutError = "unknown graph schema '" + Result.SchemaName + "'";
        return false;
    }

    for(const pugi::xml_node Item : Root.child("Properties").children("Property"))
    {
        Result.Properties[Item.attribute("Name").as_string()] = Item.attribute("Value").as_string();
    }
    for(const pugi::xml_node Item : Root.child("Variables").children("Variable"))
    {
        Result.Variables.push_back({ Item.attribute("Name").as_string(), Item.attribute("Type").as_string(),
                                     Item.attribute("Default").as_string() });
    }

    ReadNodesAndLinks(Root, *S, Result, Warnings);

    for(const pugi::xml_node Item : Root.child("Comments").children("Comment"))
    {
        Comment Loaded;
        Loaded.X      = ReadFloat(Item, "X");
        Loaded.Y      = ReadFloat(Item, "Y");
        Loaded.Width  = ReadFloat(Item, "Width");
        Loaded.Height = ReadFloat(Item, "Height");
        Loaded.Text   = Item.attribute("Text").as_string();
        Result.Comments.push_back(std::move(Loaded));
    }

    Result.Name = OutGraph.Name;
    OutGraph = std::move(Result);
    if(OutWarnings) { OutWarnings->insert(OutWarnings->end(), Warnings.begin(), Warnings.end()); }
    return true;
}

// === Files ======================================================================

bool LoadGraphFile(const std::string& InPath, Graph& OutGraph, std::string& OutError, std::vector<std::string>* OutWarnings)
{
    std::string Text;
    if(!ReadWholeFile(InPath, Text))
    {
        OutError = "could not read '" + InPath + "'";
        return false;
    }

    Graph Loaded;
    if(!ReadGraphText(Text, Loaded, OutError, OutWarnings))
    {
        OutError = "'" + InPath + "': " + OutError;
        return false;
    }

    /* The extension and the attribute must agree -- a mismatch means the file
     * was renamed into the wrong kind, and the output would land under the
     * wrong extension. A bare .nmgraph has only the attribute to go on. */
    const std::string Tag = GetGraphFileTag(InPath);
    const Schema* ByPath = Tag.empty() ? nullptr : FindSchemaByFileTag(Tag);
    if(ByPath && ByPath->GetName() != Loaded.SchemaName)
    {
        OutError = "'" + InPath + "' is named as a " + ByPath->GetName() + " graph (." + Tag + GRAPH_EXTENSION
                 + ") but holds a " + Loaded.SchemaName + " graph";
        return false;
    }
    if(!ByPath && OutWarnings)
    {
        const Schema* ByAttribute = FindSchema(Loaded.SchemaName);
        OutWarnings->push_back("'" + InPath + "' has no schema in its name; rename it to *."
                               + (ByAttribute ? ByAttribute->GetFileTag() : std::string("<tag>")) + GRAPH_EXTENSION);
    }

    Loaded.Name = GetGraphBaseName(InPath);
    OutGraph = std::move(Loaded);
    return true;
}

bool WriteTextFileIfChanged(const std::string& InPath, const std::string& InText, std::string& OutError)
{
    std::string Existing;
    if(ReadWholeFile(InPath, Existing) && Existing == InText) { return true; }

    const size_t Slash = InPath.find_last_of("/\\");
    if(Slash != std::string::npos && Slash > 0)
    {
        EngineCore::appCreateDirectories(InPath.substr(0, Slash));
    }

    const std::string Temp = InPath + TEMP_SUFFIX;
    if(!WriteWholeFile(Temp, InText))
    {
        EngineCore::appFileDelete(Temp);
        OutError = "could not write '" + Temp + "'";
        return false;
    }

    std::string RenameError;
    if(!RenameReplacing(Temp, InPath, RenameError))
    {
        EngineCore::appFileDelete(Temp);
        OutError = "could not replace '" + InPath + "': " + RenameError;
        return false;
    }
    return true;
}

bool SaveGraphFile(const std::string& InPath, const Graph& InGraph, std::string& OutError)
{
    return WriteTextFileIfChanged(InPath, WriteGraphText(InGraph), OutError);
}

bool GenerateGraphOutput(const std::string& InGraphPath, CompileResult& OutResult, std::string& OutError)
{
    Graph Loaded;
    std::vector<std::string> Warnings;
    if(!LoadGraphFile(InGraphPath, Loaded, OutError, &Warnings)) { return false; }
    for(const std::string& Each : Warnings) { OutResult.Warning(NodeGuid(), std::string(), Each); }

    CompileGraph(Loaded, OutResult);
    if(OutResult.HasErrors()) { return true; }

    const Schema* S = FindSchema(Loaded.SchemaName);
    if(!WriteTextFileIfChanged(GetGraphOutputPath(InGraphPath, *S), OutResult.Code, OutError)) { return false; }
    for(const auto& Companion : OutResult.Companions)
    {
        if(!WriteTextFileIfChanged(StripGraphExtension(InGraphPath) + Companion.first, Companion.second, OutError)) { return false; }
    }
    return true;
}

// === Clipboard ==================================================================

std::string WriteGraphFragment(const Graph& InGraph, const std::vector<NodeGuid>& InSelection)
{
    const std::set<NodeGuid> Selected(InSelection.begin(), InSelection.end());

    std::vector<const Node*> Nodes;
    for(const Node& Each : InGraph.Nodes)
    {
        if(Selected.count(Each.Id)) { Nodes.push_back(&Each); }
    }

    std::vector<Link> Links;
    for(const Link& Each : InGraph.Links)
    {
        if(Selected.count(Each.From.Node) && Selected.count(Each.To.Node)) { Links.push_back(Each); }
    }

    pugi::xml_document Doc;
    pugi::xml_node Root = Doc.append_child(FRAGMENT_ROOT);
    Root.append_attribute("Version") = GRAPH_FILE_FORMAT;
    Root.append_attribute("Schema")  = InGraph.SchemaName.c_str();
    WriteNodesAndLinks(Root, Nodes, Links);
    return DocumentText(Doc);
}

bool PasteGraphFragment(const std::string& InText, Graph& InOutGraph, const float InOffsetX, const float InOffsetY,
                        std::vector<NodeGuid>& OutNewNodes)
{
    pugi::xml_document Doc;
    if(!Doc.load_buffer(InText.data(), InText.size(), pugi::parse_default, pugi::encoding_utf8)) { return false; }

    const pugi::xml_node Root = Doc.child(FRAGMENT_ROOT);
    const Schema* S = FindSchema(InOutGraph.SchemaName);
    if(!Root || !S || InOutGraph.SchemaName != Root.attribute("Schema").as_string()) { return false; }

    Graph Fragment;
    std::vector<std::string> Warnings;
    ReadNodesAndLinks(Root, *S, Fragment, Warnings);

    std::map<NodeGuid, NodeGuid> Remap;
    for(Node& Each : Fragment.Nodes)
    {
        const NodeGuid Fresh = NodeGuid::Generate();
        Remap[Each.Id] = Fresh;
        Each.Id = Fresh;
        Each.X += InOffsetX;
        Each.Y += InOffsetY;

        /* A placeholder carries its old Id inside the kept XML too. */
        if(Each.IsPlaceholder())
        {
            pugi::xml_document Kept;
            if(Kept.load_string(Each.PlaceholderXml.c_str()) && Kept.first_child())
            {
                Kept.first_child().attribute("Id").set_value(GuidToString(Fresh).c_str());
                Each.PlaceholderXml = ElementText(Kept.first_child());
            }
        }
    }

    OutNewNodes.clear();
    for(Node& Each : Fragment.Nodes)
    {
        OutNewNodes.push_back(Each.Id);
        InOutGraph.Nodes.push_back(std::move(Each));
    }
    for(Link& Each : Fragment.Links)
    {
        Each.From.Node = Remap[Each.From.Node];
        Each.To.Node   = Remap[Each.To.Node];
        InOutGraph.Links.push_back(std::move(Each));
    }
    return true;
}
}
