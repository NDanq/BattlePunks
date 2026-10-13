#include "Public/AICore/BehaviorTreeFile.h"

#if WITH_UNCOOKED   // every build but Shipping: the editor, and a game run uncooked

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sstream>

#include "pugixml.hpp"

#include "Public/Core/CoreArchive.h"
#include "Public/Core/CorePlatform.h"

/* The .nmbt XML: BehaviorTreeFile.h has the format. */
namespace AICore
{
    namespace
    {
        constexpr int FORMAT = 1;

        std::string FormatFloat(const float InValue)
        {
            char Buffer[32];
            std::snprintf(Buffer, sizeof(Buffer), "%g", double(InValue));
            return Buffer;
        }

        float ParseFloat(const pugi::xml_attribute& InAttribute, const float InDefault)
        {
            if(!InAttribute) { return InDefault; }
            char* End = nullptr;
            const double Value = std::strtod(InAttribute.value(), &End);
            return End && End != InAttribute.value() ? float(Value) : InDefault;
        }

        void ReadParams(const pugi::xml_node& InNode, std::vector<BTParam>& OutParams, std::initializer_list<const char*> InSkip)
        {
            for(pugi::xml_attribute Attribute = InNode.first_attribute(); Attribute; Attribute = Attribute.next_attribute())
            {
                bool bSkip = false;
                for(const char* Skip : InSkip) { if(std::strcmp(Attribute.name(), Skip) == 0) { bSkip = true; } }
                if(bSkip) { continue; }
                BTParam Param;
                Param.Name = Attribute.name();
                Param.Text = Attribute.value();
                OutParams.push_back(Param);
            }
        }

        bool ReadConsideration(const pugi::xml_node& InNode, BTConsideration& OutConsideration, std::string& OutError)
        {
            OutConsideration.Input    = InNode.attribute("Input").value();
            OutConsideration.Min      = ParseFloat(InNode.attribute("Min"), 0.0f);
            OutConsideration.Max      = ParseFloat(InNode.attribute("Max"), 1.0f);
            OutConsideration.Slope    = ParseFloat(InNode.attribute("Slope"), 1.0f);
            OutConsideration.Exponent = ParseFloat(InNode.attribute("Exponent"), 2.0f);
            OutConsideration.XShift   = ParseFloat(InNode.attribute("XShift"), 0.0f);
            OutConsideration.YShift   = ParseFloat(InNode.attribute("YShift"), 0.0f);
            OutConsideration.Weight   = ParseFloat(InNode.attribute("Weight"), 1.0f);
            if(const pugi::xml_attribute Curve = InNode.attribute("Curve"))
            {
                OutConsideration.Curve = FindBTCurve(Curve.value());
                if(OutConsideration.Curve == EBTCurve::Count) { OutError = std::string("an unknown curve '") + Curve.value() + "'"; return false; }
            }
            if(const pugi::xml_attribute Points = InNode.attribute("Points"))
            {
                /* "x,y x,y ...". */
                std::string Text = Points.value();
                for(char& C : Text) { if(C == ',') { C = ' '; } }
                std::istringstream Stream(Text);
                float Value = 0.0f;
                while(Stream >> Value) { OutConsideration.Points.push_back(Value); }
                if(OutConsideration.Points.size() % 2 != 0) { OutError = "a consideration's points are not x,y pairs"; return false; }
            }
            if(OutConsideration.Input.empty() && OutConsideration.Curve != EBTCurve::Constant)
            {
                OutError = "a consideration with no Input";
                return false;
            }
            return true;
        }

        /* InXml as a node of OutAsset, depth-first: its index. */
        bool ReadNode(const pugi::xml_node& InXml, BehaviorTreeAsset& OutAsset, uint32_t& OutIndex, std::string& OutError)
        {
            const uint32_t Index = uint32_t(OutAsset.Nodes.size());
            OutAsset.Nodes.emplace_back();
            {
                BTNodeDef& Node = OutAsset.Nodes.back();
                const char* Element = InXml.name();
                if(std::strcmp(Element, "Task") == 0)
                {
                    Node.Kind = EBTNodeKind::Task;
                    Node.Type = InXml.attribute("Type").value();
                    if(Node.Type.empty()) { OutError = "a <Task> with no Type"; return false; }
                    ReadParams(InXml, Node.Params, { "Type", "Name", "Weight" });
                }
                else if(const EBTComposite Composite = FindBTComposite(Element); Composite != EBTComposite::Count)
                {
                    Node.Kind = EBTNodeKind::Composite;
                    Node.Composite = Composite;
                    Node.Type = GetBTCompositeName(Composite);
                    ReadParams(InXml, Node.Params, { "Name", "Weight" });
                }
                else if(FindBTTask(Element))
                {
                    /* <MoveTo Key="..."/>: a task by its own name. */
                    Node.Kind = EBTNodeKind::Task;
                    Node.Type = Element;
                    ReadParams(InXml, Node.Params, { "Name", "Weight" });
                }
                else
                {
                    OutError = std::string("<") + Element + ">: not a composite, and no task is registered by that name";
                    return false;
                }
                Node.Name   = InXml.attribute("Name").value();
                Node.Weight = ParseFloat(InXml.attribute("Weight"), 1.0f);
            }

            std::vector<uint32_t> Children;
            for(pugi::xml_node Child = InXml.first_child(); Child; Child = Child.next_sibling())
            {
                if(Child.type() != pugi::node_element) { continue; }
                const char* Name = Child.name();
                if(std::strcmp(Name, "Decorator") == 0)
                {
                    BTDecoratorDef Decorator;
                    Decorator.Type  = Child.attribute("Type").value();
                    Decorator.Abort = FindBTAbortMode(Child.attribute("Abort").value());
                    ReadParams(Child, Decorator.Params, { "Type", "Abort" });
                    OutAsset.Nodes[Index].Decorators.push_back(Decorator);
                }
                else if(std::strcmp(Name, "Service") == 0)
                {
                    BTServiceDef Service;
                    Service.Type     = Child.attribute("Type").value();
                    Service.Interval = ParseFloat(Child.attribute("Interval"), 0.5f);
                    ReadParams(Child, Service.Params, { "Type", "Interval" });
                    OutAsset.Nodes[Index].Services.push_back(Service);
                }
                else if(std::strcmp(Name, "Consideration") == 0)
                {
                    BTConsideration Consideration;
                    if(!ReadConsideration(Child, Consideration, OutError)) { return false; }
                    OutAsset.Nodes[Index].Considerations.push_back(Consideration);
                }
                else
                {
                    uint32_t ChildIndex = 0;
                    if(!ReadNode(Child, OutAsset, ChildIndex, OutError)) { return false; }
                    Children.push_back(ChildIndex);
                }
            }
            OutAsset.Nodes[Index].Children = Children;
            OutIndex = Index;
            return true;
        }

        void WriteParams(pugi::xml_node& InNode, const std::vector<BTParam>& InParams)
        {
            for(const BTParam& Param : InParams) { InNode.append_attribute(Param.Name.c_str()) = Param.Text.c_str(); }
        }

        void WriteNode(pugi::xml_node& InParent, const BehaviorTreeAsset& InAsset, const uint32_t InIndex)
        {
            const BTNodeDef& Node = InAsset.Nodes[InIndex];
            pugi::xml_node Xml;
            if(Node.Kind == EBTNodeKind::Composite)
            {
                Xml = InParent.append_child(GetBTCompositeName(Node.Composite));
            }
            else
            {
                Xml = InParent.append_child("Task");
                Xml.append_attribute("Type") = Node.Type.c_str();
            }
            if(!Node.Name.empty()) { Xml.append_attribute("Name") = Node.Name.c_str(); }
            if(Node.Weight != 1.0f) { Xml.append_attribute("Weight") = FormatFloat(Node.Weight).c_str(); }
            WriteParams(Xml, Node.Params);

            for(const BTConsideration& C : Node.Considerations)
            {
                pugi::xml_node Out = Xml.append_child("Consideration");
                if(!C.Input.empty()) { Out.append_attribute("Input") = C.Input.c_str(); }
                if(C.Min != 0.0f) { Out.append_attribute("Min") = FormatFloat(C.Min).c_str(); }
                if(C.Max != 1.0f) { Out.append_attribute("Max") = FormatFloat(C.Max).c_str(); }
                if(C.Curve != EBTCurve::Linear) { Out.append_attribute("Curve") = GetBTCurveName(C.Curve); }
                if(C.Slope != 1.0f) { Out.append_attribute("Slope") = FormatFloat(C.Slope).c_str(); }
                if(C.Exponent != 2.0f) { Out.append_attribute("Exponent") = FormatFloat(C.Exponent).c_str(); }
                if(C.XShift != 0.0f) { Out.append_attribute("XShift") = FormatFloat(C.XShift).c_str(); }
                if(C.YShift != 0.0f) { Out.append_attribute("YShift") = FormatFloat(C.YShift).c_str(); }
                if(C.Weight != 1.0f) { Out.append_attribute("Weight") = FormatFloat(C.Weight).c_str(); }
                if(!C.Points.empty())
                {
                    std::string Text;
                    for(size_t Index = 0; Index + 1 < C.Points.size(); Index += 2)
                    {
                        if(!Text.empty()) { Text += ' '; }
                        Text += FormatFloat(C.Points[Index]) + "," + FormatFloat(C.Points[Index + 1]);
                    }
                    Out.append_attribute("Points") = Text.c_str();
                }
            }
            for(const BTDecoratorDef& D : Node.Decorators)
            {
                pugi::xml_node Out = Xml.append_child("Decorator");
                Out.append_attribute("Type") = D.Type.c_str();
                if(D.Abort != EBTAbortMode::None) { Out.append_attribute("Abort") = GetBTAbortModeName(D.Abort); }
                WriteParams(Out, D.Params);
            }
            for(const BTServiceDef& S : Node.Services)
            {
                pugi::xml_node Out = Xml.append_child("Service");
                Out.append_attribute("Type") = S.Type.c_str();
                Out.append_attribute("Interval") = FormatFloat(S.Interval).c_str();
                WriteParams(Out, S.Params);
            }
            for(const uint32_t Child : Node.Children) { WriteNode(Xml, InAsset, Child); }
        }

        bool WriteTextFile(const std::string& InDiskPath, const void* InBytes, const size_t InCount)
        {
            EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
            Ar.SerializeBytes(const_cast<void*>(InBytes), InCount);
            return EngineCore::SaveArchiveToFile(InDiskPath, Ar);
        }
    }

    bool ParseBehaviorTreeXml(const std::string& InText, BehaviorTreeAsset& OutAsset, std::string* OutError, const bool bInResolve)
    {
        const auto Fail = [OutError](const std::string& InWhy) { if(OutError) { *OutError = InWhy; } return false; };
        pugi::xml_document Document;
        const pugi::xml_parse_result Parsed = Document.load_buffer(InText.data(), InText.size());
        if(!Parsed) { return Fail(std::string("is not valid XML: ") + Parsed.description()); }
        const pugi::xml_node Root = Document.child("BehaviorTree");
        if(!Root) { return Fail("has no <BehaviorTree>"); }
        if(Root.attribute("Format").as_int(FORMAT) > FORMAT) { return Fail("was written by a newer build"); }

        BehaviorTreeAsset Asset;
        Asset.Name = Root.attribute("Name").value();
        if(const pugi::xml_node Board = Root.child("Blackboard"))
        {
            for(pugi::xml_node Key = Board.child("Key"); Key; Key = Key.next_sibling("Key"))
            {
                BlackboardKey Declared;
                Declared.Name = Key.attribute("Name").value();
                Declared.Type = FindBlackboardType(Key.attribute("Type").value());
                Declared.Default = Key.attribute("Default").value();
                if(Declared.Name.empty()) { return Fail("a blackboard key with no Name"); }
                if(Declared.Type == EBlackboardType::Count) { return Fail("key '" + Declared.Name + "' has an unknown Type"); }
                Asset.Keys.push_back(Declared);
            }
        }

        pugi::xml_node RootNode;
        for(pugi::xml_node Child = Root.first_child(); Child; Child = Child.next_sibling())
        {
            if(Child.type() != pugi::node_element || std::strcmp(Child.name(), "Blackboard") == 0) { continue; }
            if(RootNode) { return Fail("has more than one root node"); }
            RootNode = Child;
        }
        if(!RootNode) { return Fail("has no root node"); }

        std::string Error;
        uint32_t RootIndex = 0;
        if(!ReadNode(RootNode, Asset, RootIndex, Error)) { return Fail(Error); }
        if(bInResolve && !Asset.Resolve(&Error)) { return Fail(Error); }
        OutAsset = std::move(Asset);
        return true;
    }

    std::string WriteBehaviorTreeXml(const BehaviorTreeAsset& InAsset)
    {
        pugi::xml_document Document;
        pugi::xml_node Declaration = Document.append_child(pugi::node_declaration);
        Declaration.append_attribute("version") = "1.0";
        Declaration.append_attribute("encoding") = "UTF-8";
        pugi::xml_node Root = Document.append_child("BehaviorTree");
        Root.append_attribute("Format") = FORMAT;
        Root.append_attribute("Name") = InAsset.Name.c_str();
        if(!InAsset.Keys.empty())
        {
            pugi::xml_node Board = Root.append_child("Blackboard");
            for(const BlackboardKey& Key : InAsset.Keys)
            {
                pugi::xml_node Out = Board.append_child("Key");
                Out.append_attribute("Name") = Key.Name.c_str();
                Out.append_attribute("Type") = GetBlackboardTypeName(Key.Type);
                if(!Key.Default.empty()) { Out.append_attribute("Default") = Key.Default.c_str(); }
            }
        }
        if(!InAsset.Nodes.empty()) { WriteNode(Root, InAsset, 0); }
        std::ostringstream Stream;
        Document.save(Stream, "  ", pugi::format_default, pugi::encoding_utf8);
        return Stream.str();
    }

    bool SaveBehaviorTreeXml(const std::string& InDiskPath, const BehaviorTreeAsset& InAsset, std::string* OutError)
    {
        const std::string Text = WriteBehaviorTreeXml(InAsset);
        if(!WriteTextFile(InDiskPath, Text.data(), Text.size()))
        {
            if(OutError) { *OutError = "could not be written"; }
            return false;
        }
        BehaviorTreeAssetCache::Invalidate("");
        return true;
    }

    bool CookBehaviorTreeFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError)
    {
        std::vector<uint8_t> Bytes;
        if(!EngineCore::appFileReadAll(InSourceDiskPath, Bytes))
        {
            if(OutError) { *OutError = "cannot be read"; }
            return false;
        }
        BehaviorTreeAsset Asset;
        if(!ReadBehaviorTree(Bytes, Asset, OutError)) { return false; }
        const std::vector<uint8_t> Cooked = WriteBehaviorTreeBinary(Asset);
        if(!WriteTextFile(InOutDiskPath, Cooked.data(), Cooked.size()))
        {
            if(OutError) { *OutError = "could not write " + InOutDiskPath; }
            return false;
        }
        return true;
    }
}

#endif // WITH_UNCOOKED
