#include "Public/AICore/BehaviorTree.h"

#include <algorithm>
#include <cstdlib>
#include <cstring>

#include "Private/AICore/BTInternal.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/World.h"

/*
 * The behavior tree runtime: resolving an asset, and ticking an instance.
 * BehaviorTree.h has the model; this file is the memory-based tick -- each
 * node keeps whether it is active and which child it is on, so a tick walks
 * only the running branch.
 */
namespace AICore
{
    // === The asset ============================================================

    const BTParam* BTNodeDef::FindParam(const char* InName) const
    {
        for(const BTParam& Param : Params)
        {
            if(Param.Name == InName) { return &Param; }
        }
        return nullptr;
    }

    int32_t BehaviorTreeAsset::FindKey(const char* InName) const
    {
        for(size_t Index = 0; Index < Keys.size(); ++Index)
        {
            if(Keys[Index].Name == InName) { return int32_t(Index); }
        }
        return Blackboard::INVALID_KEY;
    }

    namespace
    {
        void ResolveParams(const BehaviorTreeAsset& InAsset, std::vector<BTParam>& InOutParams)
        {
            for(BTParam& Param : InOutParams)
            {
                char* End = nullptr;
                Param.Number = std::strtod(Param.Text.c_str(), &End);
                Param.bIsNumber = !Param.Text.empty() && End && *End == '\0';
                if(!Param.bIsNumber) { Param.Number = 0.0; }
                Param.bBool = Param.Text == "true" || Param.Text == "True" || Param.Text == "1";
                Param.Key = InAsset.FindKey(Param.Text.c_str());
            }
        }
    }

    bool BehaviorTreeAsset::Resolve(std::string* OutError)
    {
        const auto Fail = [this, OutError](const std::string& InWhy) { bResolved = false; if(OutError) { *OutError = InWhy; } return false; };
        bResolved = false;
        DecoratorSlots = 0;
        ServiceSlots = 0;
        if(Nodes.empty()) { return Fail("the tree has no root node"); }

        for(BTNodeDef& Node : Nodes) { Node.Parent = BT_NO_NODE; }
        for(uint32_t Index = 0; Index < Nodes.size(); ++Index)
        {
            BTNodeDef& Node = Nodes[Index];
            const std::string Where = "node " + std::to_string(Index) + " (" + Node.GetDisplayName() + ")";
            if(Node.Kind == EBTNodeKind::Composite)
            {
                if(Node.Composite >= EBTComposite::Count) { return Fail(Where + ": an unknown composite"); }
                if(Node.Type.empty()) { Node.Type = GetBTCompositeName(Node.Composite); }
                if(Node.Children.empty()) { return Fail(Where + ": a composite with no children"); }
                if(Node.Children.size() > 32) { return Fail(Where + ": more than 32 children"); }
                Node.Task = nullptr;
            }
            else
            {
                if(!Node.Children.empty()) { return Fail(Where + ": a task with children"); }
                Node.Task = FindBTTask(Node.Type.c_str());
                if(!Node.Task) { return Fail(Where + ": no task type '" + Node.Type + "' is registered"); }
            }
            for(size_t Child = 0; Child < Node.Children.size(); ++Child)
            {
                const uint32_t C = Node.Children[Child];
                if(C == 0 || C >= Nodes.size()) { return Fail(Where + ": a child out of range"); }
                if(Nodes[C].Parent != BT_NO_NODE) { return Fail(Where + ": a node with two parents"); }
                Nodes[C].Parent = Index;
                Nodes[C].IndexInParent = uint32_t(Child);
            }
            for(BTDecoratorDef& Decorator : Node.Decorators)
            {
                Decorator.Impl = FindBTDecorator(Decorator.Type.c_str());
                if(!Decorator.Impl) { return Fail(Where + ": no decorator type '" + Decorator.Type + "' is registered"); }
                Decorator.Slot = DecoratorSlots++;
                ResolveParams(*this, Decorator.Params);
            }
            for(BTServiceDef& Service : Node.Services)
            {
                Service.Impl = FindBTService(Service.Type.c_str());
                if(!Service.Impl) { return Fail(Where + ": no service type '" + Service.Type + "' is registered"); }
                Service.Slot = ServiceSlots++;
                ResolveParams(*this, Service.Params);
            }
            for(BTConsideration& Consideration : Node.Considerations)
            {
                std::string Input = Consideration.Input;
                Consideration.bDistance = false;
                if(Input.size() > 10 && Input.compare(0, 9, "distance(") == 0 && Input.back() == ')')
                {
                    Consideration.bDistance = true;
                    Input = Input.substr(9, Input.size() - 10);
                }
                Consideration.Key = FindKey(Input.c_str());
                if(Consideration.Key < 0 && !Consideration.Input.empty())
                {
                    return Fail(Where + ": a consideration reads '" + Input + "', which the blackboard does not declare");
                }
            }
            ResolveParams(*this, Node.Params);
        }
        for(uint32_t Index = 1; Index < Nodes.size(); ++Index)
        {
            if(Nodes[Index].Parent == BT_NO_NODE) { return Fail("node " + std::to_string(Index) + " has no parent"); }
        }
        /* Depths, parents first: the nodes are depth-first, so a parent's
         * index is always below its children's. */
        Nodes[0].Depth = 0;
        for(uint32_t Index = 1; Index < Nodes.size(); ++Index)
        {
            if(Nodes[Index].Parent >= Index) { return Fail("node " + std::to_string(Index) + " is not depth-first"); }
            Nodes[Index].Depth = Nodes[Nodes[Index].Parent].Depth + 1;
        }
        bResolved = true;
        return true;
    }

    std::vector<std::string> BehaviorTreeAsset::Validate() const
    {
        std::vector<std::string> Problems;
        for(const BTNodeDef& Node : Nodes)
        {
            if(Node.Kind == EBTNodeKind::Composite && Node.Composite == EBTComposite::UtilitySelector)
            {
                for(const uint32_t Child : Node.Children)
                {
                    if(Child < Nodes.size() && Nodes[Child].Considerations.empty() && Nodes[Child].Weight <= 0.0f)
                    {
                        Problems.push_back(std::string("'") + Nodes[Child].GetDisplayName() + "' under a utility selector can never score");
                    }
                }
            }
        }
        return Problems;
    }

    // === The context ==========================================================

    const BTParam* BTContext::FindParam(const char* InName) const
    {
        for(const BTParam& Param : Params)
        {
            if(Param.Name == InName) { return &Param; }
        }
        return nullptr;
    }

    double BTContext::GetNumber(const char* InName, const double InDefault) const
    {
        const BTParam* Param = FindParam(InName);
        if(!Param) { return InDefault; }
        if(Param->bIsNumber) { return Param->Number; }
        if(Param->Key >= 0) { return Board.GetValue(Param->Key).AsNumber(); }
        return InDefault;
    }

    bool BTContext::GetBool(const char* InName, const bool bInDefault) const
    {
        const BTParam* Param = FindParam(InName);
        if(!Param) { return bInDefault; }
        if(Param->Key >= 0) { return Board.GetValue(Param->Key).AsNumber() != 0.0; }
        if(Param->Text == "false" || Param->Text == "False" || Param->Text == "0") { return false; }
        return Param->bBool || bInDefault;
    }

    const char* BTContext::GetString(const char* InName, const char* InDefault) const
    {
        const BTParam* Param = FindParam(InName);
        return Param ? Param->Text.c_str() : InDefault;
    }

    int32_t BTContext::GetKey(const char* InName) const
    {
        const BTParam* Param = FindParam(InName);
        if(!Param) { return Blackboard::INVALID_KEY; }
        /* A key the game added after the tree loaded resolves here. */
        return Param->Key >= 0 ? Param->Key : Board.FindKey(Param->Text.c_str());
    }

    float BTContext::Random() const { return Instance.NextRandom(); }

    // === The instance =========================================================

    BTInstance::BTInstance(std::shared_ptr<const BehaviorTreeAsset> InAsset, GameCore::World& InWorld, const GameCore::EntityID InSelf,
                           AIWorld* InAI, const uint32_t InSeed)
        : Asset(std::move(InAsset)), World(InWorld), Self(InSelf), AI(InAI), RandomState(InSeed ? InSeed : 1)
    {
        const size_t Nodes = Asset ? Asset->Nodes.size() : 0;
        Memory.assign(Nodes, BTNodeMemory());
        ActiveSeconds.assign(Nodes, 0.0);
        UtilityScores.assign(Nodes, -1.0f);
        DecoratorMemory.assign(Asset ? Asset->GetDecoratorSlotCount() : 0, BTNodeMemory());
        ServiceMemory.assign(Asset ? Asset->GetServiceSlotCount() : 0, BTNodeMemory());
        if(Asset) { Board.Declare(Asset->Keys); }
    }

    BTInstance::~BTInstance()
    {
        /* Tasks let go of what they hold (a MoveTo's agent). */
        Abort();
    }

    float BTInstance::NextRandom()
    {
        RandomState ^= RandomState << 13;
        RandomState ^= RandomState >> 17;
        RandomState ^= RandomState << 5;
        return float(RandomState & 0xFFFFFF) / float(0x1000000);
    }

    BTContext BTInstance::MakeContext(const uint32_t InNode, const std::vector<BTParam>& InParams, BTNodeMemory& InMemory, const float InDeltaSeconds)
    {
        return BTContext{ World, Self, Board, *this, AI, Asset->Nodes[InNode], InMemory, InParams, InDeltaSeconds, Clock };
    }

    EBTStatus BTInstance::Tick(const float InDeltaSeconds)
    {
        if(!Asset || Asset->Nodes.empty() || !Asset->IsResolved()) { return LastStatus = EBTStatus::Failure; }
        Clock += double(InDeltaSeconds);
        ++Ticks;
        for(size_t Index = 0; Index < Memory.size(); ++Index)
        {
            if(Memory[Index].bActive) { ActiveSeconds[Index] += double(InDeltaSeconds); }
        }
        LastStatus = TickNode(0, InDeltaSeconds);
        return LastStatus;
    }

    void BTInstance::Abort()
    {
        if(Asset && !Asset->Nodes.empty()) { AbortNode(0); }
    }

    bool BTInstance::ConditionsPass(const uint32_t InNode, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        for(const BTDecoratorDef& Decorator : Node.Decorators)
        {
            if(!Decorator.Impl->IsCondition()) { continue; }
            BTContext Context = MakeContext(InNode, Decorator.Params, DecoratorMemory[Decorator.Slot], InDeltaSeconds);
            if(!Decorator.Impl->Evaluate(Context)) { return false; }
        }
        return true;
    }

    bool BTInstance::SelfAbortFails(const uint32_t InNode, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        const uint64_t Revision = Board.GetRevision();
        for(const BTDecoratorDef& Decorator : Node.Decorators)
        {
            if(Decorator.Abort != EBTAbortMode::Self && Decorator.Abort != EBTAbortMode::Both) { continue; }
            if(!Decorator.Impl->IsCondition() || !Decorator.Impl->ObservesBlackboard()) { continue; }
            BTNodeMemory& Mem = DecoratorMemory[Decorator.Slot];
            if(Mem.SeenRevision == Revision) { continue; }
            Mem.SeenRevision = Revision;
            BTContext Context = MakeContext(InNode, Decorator.Params, Mem, InDeltaSeconds);
            if(!Decorator.Impl->Evaluate(Context)) { return true; }
        }
        for(const BTDecoratorDef& Decorator : Node.Decorators)
        {
            BTContext Context = MakeContext(InNode, Decorator.Params, DecoratorMemory[Decorator.Slot], InDeltaSeconds);
            if(!Decorator.Impl->KeepRunning(Context)) { return true; }
        }
        return false;
    }

    uint32_t BTInstance::FindLowerPriorityAbort(const uint32_t InNode, const float InDeltaSeconds)
    {
        BTNodeMemory& Mem = Memory[InNode];
        const uint64_t Revision = Board.GetRevision();
        if(Mem.SeenRevision == Revision) { return BT_NO_NODE; }
        Mem.SeenRevision = Revision;

        const BTNodeDef& Node = Asset->Nodes[InNode];
        for(uint32_t Child = 0; Child < Mem.Child && Child < Node.Children.size(); ++Child)
        {
            const BTNodeDef& Earlier = Asset->Nodes[Node.Children[Child]];
            bool bObserves = false;
            for(const BTDecoratorDef& Decorator : Earlier.Decorators)
            {
                if((Decorator.Abort == EBTAbortMode::LowerPriority || Decorator.Abort == EBTAbortMode::Both)
                   && Decorator.Impl->IsCondition() && Decorator.Impl->ObservesBlackboard())
                {
                    bObserves = true;
                }
            }
            if(bObserves && ConditionsPass(Node.Children[Child], InDeltaSeconds)) { return Child; }
        }
        return BT_NO_NODE;
    }

    void BTInstance::TickServices(const uint32_t InNode, const float InDeltaSeconds, const bool bInEntering)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        for(const BTServiceDef& Service : Node.Services)
        {
            BTNodeMemory& Mem = ServiceMemory[Service.Slot];
            BTContext Context = MakeContext(InNode, Service.Params, Mem, InDeltaSeconds);
            if(bInEntering)
            {
                Mem = BTNodeMemory();
                Service.Impl->OnEnter(Context);
                Mem.Time = Clock;
            }
            if(Clock >= Mem.Time)
            {
                Service.Impl->OnTick(Context);
                Mem.Time = Clock + double(std::max(Service.Interval, 0.0f));
            }
        }
    }

    EBTStatus BTInstance::Enter(const uint32_t InNode, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];
        if(!ConditionsPass(InNode, InDeltaSeconds))
        {
            Mem.Status = EBTStatus::Failure;
            return EBTStatus::Failure;
        }

        Mem = BTNodeMemory();
        Mem.Status = EBTStatus::Running;
        Mem.bActive = 1;
        Mem.StartTime = Clock;
        Mem.SeenRevision = Board.GetRevision();
        for(const BTDecoratorDef& Decorator : Node.Decorators)
        {
            BTNodeMemory& DecoratorMem = DecoratorMemory[Decorator.Slot];
            DecoratorMem.SeenRevision = Board.GetRevision();
            BTContext Context = MakeContext(InNode, Decorator.Params, DecoratorMem, InDeltaSeconds);
            Decorator.Impl->OnEnter(Context);
        }
        TickServices(InNode, InDeltaSeconds, true);

        if(Node.Kind == EBTNodeKind::Task)
        {
            BTContext Context = MakeContext(InNode, Node.Params, Mem, InDeltaSeconds);
            const EBTStatus Status = Node.Task->OnEnter(Context);
            if(Status != EBTStatus::Running) { return Finish(InNode, Status, InDeltaSeconds); }
        }
        return EBTStatus::Running;
    }

    EBTStatus BTInstance::Finish(const uint32_t InNode, EBTStatus InStatus, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];
        Mem.bActive = 0;
        if(Node.Kind == EBTNodeKind::Task)
        {
            BTContext Context = MakeContext(InNode, Node.Params, Mem, InDeltaSeconds);
            Node.Task->OnExit(Context, InStatus);
        }
        for(size_t Index = Node.Decorators.size(); Index-- > 0;)
        {
            const BTDecoratorDef& Decorator = Node.Decorators[Index];
            BTContext Context = MakeContext(InNode, Decorator.Params, DecoratorMemory[Decorator.Slot], InDeltaSeconds);
            InStatus = Decorator.Impl->OnResult(Context, InStatus);
        }
        Mem.Status = InStatus;
        return InStatus;
    }

    void BTInstance::AbortNode(const uint32_t InNode)
    {
        BTNodeMemory& Mem = Memory[InNode];
        if(!Mem.bActive) { return; }
        const BTNodeDef& Node = Asset->Nodes[InNode];
        if(Node.Kind == EBTNodeKind::Composite)
        {
            for(const uint32_t Child : Node.Children) { AbortNode(Child); }
        }
        Mem.bActive = 0;
        if(Node.Kind == EBTNodeKind::Task)
        {
            BTContext Context = MakeContext(InNode, Node.Params, Mem, 0.0f);
            Node.Task->OnExit(Context, EBTStatus::Aborted);
        }
        for(size_t Index = Node.Decorators.size(); Index-- > 0;)
        {
            const BTDecoratorDef& Decorator = Node.Decorators[Index];
            BTContext Context = MakeContext(InNode, Decorator.Params, DecoratorMemory[Decorator.Slot], 0.0f);
            Decorator.Impl->OnResult(Context, EBTStatus::Aborted);
        }
        Mem.Status = EBTStatus::Aborted;
    }

    EBTStatus BTInstance::TickNode(const uint32_t InNode, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];

        if(!Mem.bActive)
        {
            const EBTStatus Entered = Enter(InNode, InDeltaSeconds);
            if(Entered != EBTStatus::Running || !Mem.bActive) { return Entered; }
            /* A task ticks from the next frame; a composite descends now. */
            if(Node.Kind == EBTNodeKind::Task) { return EBTStatus::Running; }
        }
        else
        {
            if(SelfAbortFails(InNode, InDeltaSeconds))
            {
                AbortNode(InNode);
                Mem.Status = EBTStatus::Failure;
                return EBTStatus::Failure;
            }
            TickServices(InNode, InDeltaSeconds, false);
        }

        EBTStatus Status;
        if(Node.Kind == EBTNodeKind::Task)
        {
            BTContext Context = MakeContext(InNode, Node.Params, Mem, InDeltaSeconds);
            Status = Node.Task->OnTick(Context);
        }
        else
        {
            Status = TickComposite(InNode, InDeltaSeconds);
        }
        if(Status != EBTStatus::Running && Mem.bActive) { return Finish(InNode, Status, InDeltaSeconds); }
        return Status;
    }

    EBTStatus BTInstance::TickComposite(const uint32_t InNode, const float InDeltaSeconds)
    {
        switch(Asset->Nodes[InNode].Composite)
        {
            case EBTComposite::Sequence:        return TickSequence(InNode, false, InDeltaSeconds);
            case EBTComposite::Selector:        return TickSequence(InNode, true, InDeltaSeconds);
            case EBTComposite::Parallel:        return TickParallel(InNode, InDeltaSeconds);
            case EBTComposite::RandomSelector:  return TickRandomSelector(InNode, InDeltaSeconds);
            case EBTComposite::UtilitySelector: return BTDetail::TickUtilitySelector(*this, InNode, InDeltaSeconds);
            default:                            return EBTStatus::Failure;
        }
    }

    EBTStatus BTInstance::TickSequence(const uint32_t InNode, const bool bInSelector, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];

        if(bInSelector)
        {
            const uint32_t Earlier = FindLowerPriorityAbort(InNode, InDeltaSeconds);
            if(Earlier != BT_NO_NODE)
            {
                if(Mem.Child < Node.Children.size()) { AbortNode(Node.Children[Mem.Child]); }
                Mem.Child = Earlier;
            }
        }

        while(Mem.Child < Node.Children.size())
        {
            const EBTStatus Status = TickNode(Node.Children[Mem.Child], InDeltaSeconds);
            if(Status == EBTStatus::Running) { return EBTStatus::Running; }
            if(!bInSelector && Status != EBTStatus::Success) { return EBTStatus::Failure; }
            if(bInSelector && Status == EBTStatus::Success) { return EBTStatus::Success; }
            ++Mem.Child;
        }
        return bInSelector ? EBTStatus::Failure : EBTStatus::Success;
    }

    EBTStatus BTInstance::TickParallel(const uint32_t InNode, const float InDeltaSeconds)
    {
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];
        BTContext Context = MakeContext(InNode, Node.Params, Mem, InDeltaSeconds);
        const bool bSucceedOnAll = std::strcmp(Context.GetString("Success", "All"), "All") == 0;
        const bool bFailOnAll    = std::strcmp(Context.GetString("Failure", "One"), "All") == 0;

        const uint32_t Count = uint32_t(Node.Children.size());
        for(uint32_t Child = 0; Child < Count; ++Child)
        {
            const uint32_t Bit = 1u << Child;
            if(Mem.Mask & Bit) { continue; }
            const EBTStatus Status = TickNode(Node.Children[Child], InDeltaSeconds);
            if(Status == EBTStatus::Running) { continue; }
            Mem.Mask |= Bit;
            if(Status == EBTStatus::Success) { ++Mem.Counter; }
            else { Mem.Values[0] += 1.0f; }
        }
        const int32_t Successes = Mem.Counter;
        const int32_t Failures  = int32_t(Mem.Values[0]);
        EBTStatus Result = EBTStatus::Running;
        if(bFailOnAll ? Failures == int32_t(Count) : Failures > 0)              { Result = EBTStatus::Failure; }
        else if(bSucceedOnAll ? Successes == int32_t(Count) : Successes > 0)    { Result = EBTStatus::Success; }
        else if(Successes + Failures == int32_t(Count))                          { Result = Successes > 0 ? EBTStatus::Success : EBTStatus::Failure; }
        if(Result != EBTStatus::Running)
        {
            for(const uint32_t Child : Node.Children) { AbortNode(Child); }
        }
        return Result;
    }

    EBTStatus BTInstance::TickRandomSelector(const uint32_t InNode, const float InDeltaSeconds)
    {
        (void)InDeltaSeconds;
        const BTNodeDef& Node = Asset->Nodes[InNode];
        BTNodeMemory& Mem = Memory[InNode];
        const uint32_t Count = uint32_t(Node.Children.size());

        const auto Pick = [&]() -> bool
        {
            float Total = 0.0f;
            for(uint32_t Child = 0; Child < Count; ++Child)
            {
                if(!(Mem.Mask & (1u << Child))) { Total += std::max(Asset->Nodes[Node.Children[Child]].Weight, 0.0f); }
            }
            if(Total <= 0.0f)
            {
                for(uint32_t Child = 0; Child < Count; ++Child)
                {
                    if(!(Mem.Mask & (1u << Child))) { Mem.Child = Child; return true; }
                }
                return false;
            }
            float Roll = NextRandom() * Total;
            for(uint32_t Child = 0; Child < Count; ++Child)
            {
                if(Mem.Mask & (1u << Child)) { continue; }
                Roll -= std::max(Asset->Nodes[Node.Children[Child]].Weight, 0.0f);
                if(Roll <= 0.0f) { Mem.Child = Child; return true; }
            }
            for(uint32_t Child = Count; Child-- > 0;)
            {
                if(!(Mem.Mask & (1u << Child))) { Mem.Child = Child; return true; }
            }
            return false;
        };

        if(!(Mem.Flags & 1))
        {
            Mem.Flags |= 1;
            if(!Pick()) { return EBTStatus::Failure; }
        }
        for(;;)
        {
            const EBTStatus Status = TickNode(Node.Children[Mem.Child], InDeltaSeconds);
            if(Status == EBTStatus::Running) { return EBTStatus::Running; }
            if(Status == EBTStatus::Success) { return EBTStatus::Success; }
            Mem.Mask |= 1u << Mem.Child;
            if(!Pick()) { return EBTStatus::Failure; }
        }
    }

    // === Introspection ========================================================

    void BTInstance::GetActivePath(std::vector<uint32_t>& OutNodes) const
    {
        OutNodes.clear();
        for(uint32_t Index = 0; Index < Memory.size(); ++Index)
        {
            if(Memory[Index].bActive) { OutNodes.push_back(Index); }
        }
    }

    float BTInstance::GetUtilityScore(const uint32_t InNode) const
    {
        return InNode < UtilityScores.size() ? UtilityScores[InNode] : -1.0f;
    }

    double BTInstance::GetNodeActiveSeconds(const uint32_t InNode) const
    {
        return InNode < ActiveSeconds.size() ? ActiveSeconds[InNode] : 0.0;
    }

    // === Helpers ==============================================================

    namespace BTDetail
    {
        EngineCore::Vector3 SelfPosition(BTInstance& InInstance)
        {
            return InInstance.GetWorld().GetEntity(InInstance.GetSelf()).GetWorldPose().Position;
        }

        bool ResolveKeyPosition(BTInstance& InInstance, const int32_t InKey, EngineCore::Vector3& OutPosition)
        {
            const Blackboard& Board = InInstance.GetBlackboard();
            if(InKey < 0 || size_t(InKey) >= Board.GetKeyCount() || !Board.IsSet(InKey)) { return false; }
            const BlackboardValue& Value = Board.GetValue(InKey);
            if(Value.Type == EBlackboardType::Vector) { OutPosition = Value.VectorValue; return true; }
            if(Value.Type == EBlackboardType::Entity)
            {
                GameCore::World& World = InInstance.GetWorld();
                if(!World.IsAlive(Value.EntityValue)) { return false; }
                OutPosition = World.GetEntity(Value.EntityValue).GetWorldPose().Position;
                return true;
            }
            return false;
        }
    }
}
