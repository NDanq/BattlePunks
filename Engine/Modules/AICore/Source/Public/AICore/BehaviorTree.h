#ifndef AICORE_BEHAVIORTREE_H
#define AICORE_BEHAVIORTREE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/Blackboard.h"
#include "Public/GameCore/EntityID.h"

namespace GameCore { class World; }

/*
 * Behavior trees: an immutable asset shared by every brain that runs it, and
 * a per-brain instance -- a flat array of node memory and a blackboard -- so
 * ticking allocates nothing (the Game AI Pro "Behavior Tree Starter Kit"'s
 * second-generation design).
 *
 * STATUS. Every node returns Success, Failure or Running (Colledanchise &
 * Ogren's contract). A Running node is resumed on the next tick; only the
 * running branch is ticked, never its siblings.
 *
 * NODES.
 *   Composites   Sequence (all, in order), Selector (the first that
 *                succeeds; "Fallback" in the literature), Parallel (every
 *                child each tick, by policy), RandomSelector (a weighted
 *                pick, then the rest), UtilitySelector (the best-scoring
 *                child by its considerations; UtilityNodes.cpp).
 *   Tasks        leaves: MoveTo, MoveToRandom, Wait, SetBlackboard,
 *                ClearBlackboard, FaceTarget, PlayAnimParam, RunScript, Log --
 *                and whatever a game registers (AICORE_DEFINE_BT_TASK).
 *   Decorators   attached to a node: conditions on entering it
 *                (BlackboardCondition, Cooldown) and changes to its result
 *                (Inverter, ForceSuccess, ForceFailure, Loop, TimeLimit).
 *   Services     attached to a composite: ticked every Interval seconds while
 *                it is active (DistanceToKey; a game registers its own).
 *
 * ABORTS (Unreal's observer aborts). A BlackboardCondition with Abort Self
 * stops its node when the condition turns false while it runs; with Abort
 * LowerPriority, a Selector running a later child switches back to this one
 * when the condition turns true. Both re-check only when the blackboard's
 * revision moved.
 */
namespace AICore
{
    class AIWorld;
    class BTInstance;

    enum class EBTStatus : uint8_t { Success = 0, Failure, Running, Aborted };
    AICORE_API const char* GetBTStatusName(EBTStatus InStatus);

    enum class EBTNodeKind : uint8_t { Composite = 0, Task };

    enum class EBTComposite : uint8_t
    {
        Sequence = 0,
        Selector,
        Parallel,
        RandomSelector,
        UtilitySelector,
        Count,
    };
    AICORE_API const char*  GetBTCompositeName(EBTComposite InType);
    /* "Sequence", "Selector" (or "Fallback"), ...; Count if none. */
    AICORE_API EBTComposite FindBTComposite(const char* InName);

    enum class EBTAbortMode : uint8_t { None = 0, Self, LowerPriority, Both };
    AICORE_API const char*  GetBTAbortModeName(EBTAbortMode InMode);
    AICORE_API EBTAbortMode FindBTAbortMode(const char* InName);

    /* A consideration's response curve (UtilityNodes.cpp has the shapes). */
    enum class EBTCurve : uint8_t
    {
        Linear = 0,       // y = Slope * (x - XShift) + YShift
        InverseLinear,    // 1 - x
        Quadratic,        // y = Slope * (x - XShift)^Exponent + YShift
        Logistic,         // y = 1 / (1 + e^(-Slope * (x - XShift))) + YShift
        Step,             // 1 past XShift, else 0
        Constant,         // YShift (a branch's floor score)
        Points,           // piecewise-linear through Points
        Count,
    };
    AICORE_API const char* GetBTCurveName(EBTCurve InCurve);
    AICORE_API EBTCurve    FindBTCurve(const char* InName);

    // ========================================================================
    // The asset
    // ========================================================================

    /* A node's, decorator's or service's property, kept as written and
     * pre-parsed: as a number, a bool, and the blackboard key it names. */
    struct BTParam
    {
        std::string Name;
        std::string Text;
        double      Number = 0.0;
        bool        bBool  = false;
        bool        bIsNumber = false;   // Text parsed as a number
        int32_t     Key    = Blackboard::INVALID_KEY;   // if Text names a declared key
    };

    /* A UtilitySelector child's input, normalised and shaped. */
    struct BTConsideration
    {
        /* What it reads: a blackboard key ("Health"), or a distance to one
         * ("distance(TargetActor)") from the brain's entity. */
        std::string Input;
        float    Min = 0.0f;           // Input maps Min..Max onto 0..1, clamped
        float    Max = 1.0f;
        EBTCurve Curve = EBTCurve::Linear;
        float    Slope    = 1.0f;
        float    Exponent = 2.0f;
        float    XShift   = 0.0f;
        float    YShift   = 0.0f;
        std::vector<float> Points;     // x0, y0, x1, y1 ... for Points
        float    Weight = 1.0f;        // the score is raised to this power

        // Resolved
        int32_t  Key = Blackboard::INVALID_KEY;
        bool     bDistance = false;

        /* The curve at x (0..1), clamped to 0..1. */
        AICORE_API float Evaluate(float InX) const;
    };

    class BTTask;
    class BTDecorator;
    class BTService;

    struct BTDecoratorDef
    {
        std::string          Type;
        std::vector<BTParam> Params;
        EBTAbortMode         Abort = EBTAbortMode::None;
        const BTDecorator*   Impl  = nullptr;   // resolved
        uint32_t             Slot  = 0;         // its memory, in BTInstance
    };

    struct BTServiceDef
    {
        std::string          Type;
        std::vector<BTParam> Params;
        float                Interval = 0.5f;
        const BTService*     Impl = nullptr;    // resolved
        uint32_t             Slot = 0;
    };

    enum : uint32_t { BT_NO_NODE = 0xFFFFFFFFu };

    struct BTNodeDef
    {
        EBTNodeKind  Kind      = EBTNodeKind::Task;
        EBTComposite Composite = EBTComposite::Sequence;
        std::string  Type;     // the composite's or task's registered name
        std::string  Name;     // as authored, for the debugger ("" : Type)
        std::vector<BTParam>         Params;
        std::vector<uint32_t>        Children;
        std::vector<BTDecoratorDef>  Decorators;
        std::vector<BTServiceDef>    Services;
        std::vector<BTConsideration> Considerations;   // as a UtilitySelector's child
        float Weight = 1.0f;                            // as a Random/UtilitySelector's child

        // Resolved
        const BTTask* Task   = nullptr;
        uint32_t      Parent = BT_NO_NODE;
        uint32_t      IndexInParent = 0;
        uint32_t      Depth  = 0;

        /* The param so named, or null. */
        AICORE_API const BTParam* FindParam(const char* InName) const;
        const char* GetDisplayName() const { return Name.empty() ? Type.c_str() : Name.c_str(); }
    };

    class AICORE_API BehaviorTreeAsset
    {
    public:
        std::string                Name;
        std::vector<BlackboardKey> Keys;
        /* Depth-first; [0] is the root. */
        std::vector<BTNodeDef>     Nodes;

        /*
         * Links node, decorator and service types to the registry, parents
         * and depths, memory slots, and params to blackboard keys. Every load
         * does it. False, with OutError naming the first problem: an unknown
         * type, a composite with no children, a task with some.
         */
        bool Resolve(std::string* OutError = nullptr);
        bool IsResolved() const { return bResolved; }

        uint32_t GetDecoratorSlotCount() const { return DecoratorSlots; }
        uint32_t GetServiceSlotCount() const   { return ServiceSlots; }

        int32_t FindKey(const char* InName) const;

        /* Every asset path a tree references that must ship with it: none
         * today (trees name no assets), but the cook asks. */
        std::vector<std::string> Validate() const;

    private:
        bool     bResolved = false;
        uint32_t DecoratorSlots = 0;
        uint32_t ServiceSlots = 0;
    };

    // ========================================================================
    // Running
    // ========================================================================

    /* What a node keeps between ticks: fixed, so an instance is one array. */
    struct BTNodeMemory
    {
        EBTStatus Status  = EBTStatus::Success;   // the last result
        uint8_t   bActive = 0;                    // entered and not finished
        uint8_t   Flags   = 0;                    // free for the node
        uint32_t  Child   = 0;                    // a composite's current child
        uint32_t  Mask    = 0;                    // a composite's tried / finished children
        int32_t   Counter = 0;
        double    StartTime = 0.0;                // when it was entered
        double    Time      = 0.0;                // free for the node
        float     Values[4] = {};                 // free for the node
        EngineCore::Vector3 Vector;               // free for the node
        uint64_t  SeenRevision = 0;               // the blackboard revision last checked
    };

    /* Everything a node's code sees while it runs. */
    struct AICORE_API BTContext
    {
        GameCore::World&   World;
        GameCore::EntityID Self;
        Blackboard&        Board;
        BTInstance&        Instance;
        AIWorld*           AI;
        const BTNodeDef&   Node;
        BTNodeMemory&      Memory;
        const std::vector<BTParam>& Params;   // the node's, decorator's or service's
        float              DeltaSeconds;
        double             Time;              // the instance's clock

        const BTParam* FindParam(const char* InName) const;
        double      GetNumber(const char* InName, double InDefault = 0.0) const;
        bool        GetBool(const char* InName, bool bInDefault = false) const;
        const char* GetString(const char* InName, const char* InDefault = "") const;
        /* The blackboard key the param names, INVALID_KEY if none. */
        int32_t     GetKey(const char* InName) const;
        /* A random number in [0, 1), from the instance's own stream. */
        float       Random() const;
    };

    /* A leaf. Stateless: one object serves every node and instance of its
     * type, and keeps what it needs in BTNodeMemory. */
    class AICORE_API BTTask
    {
    public:
        virtual ~BTTask() = default;
        /* On entering: Running to be ticked, or done at once. */
        virtual EBTStatus OnEnter(BTContext& InContext) const { (void)InContext; return EBTStatus::Running; }
        virtual EBTStatus OnTick(BTContext& InContext) const { (void)InContext; return EBTStatus::Success; }
        /* On leaving, finished (Success/Failure) or Aborted. */
        virtual void OnExit(BTContext& InContext, EBTStatus InStatus) const { (void)InContext; (void)InStatus; }
    };

    class AICORE_API BTDecorator
    {
    public:
        virtual ~BTDecorator() = default;
        /* A condition: whether its node may run (on entering, and while it
         * runs for Abort Self). False from a decorator that is not one. */
        virtual bool IsCondition() const { return false; }
        virtual bool Evaluate(BTContext& InContext) const { (void)InContext; return true; }
        /* Whether Evaluate reads only the blackboard: the abort modes need it. */
        virtual bool ObservesBlackboard() const { return false; }
        virtual void OnEnter(BTContext& InContext) const { (void)InContext; }
        /* Its node finished (Success/Failure) or was aborted: the result to
         * pass on. Running starts the node again (a Loop). */
        virtual EBTStatus OnResult(BTContext& InContext, EBTStatus InStatus) const { (void)InContext; return InStatus; }
        /* While its node runs: false aborts it (a TimeLimit). */
        virtual bool KeepRunning(BTContext& InContext) const { (void)InContext; return true; }
    };

    class AICORE_API BTService
    {
    public:
        virtual ~BTService() = default;
        virtual void OnEnter(BTContext& InContext) const { (void)InContext; }
        virtual void OnTick(BTContext& InContext) const = 0;
    };

    /* What a brain asks of the game around it (AIWorld::SetHost). */
    struct BTHost
    {
        /* An animation parameter on InEntity (RenderCore's Animation
         * component): PlayAnimParam. False if it has none. */
        bool (*SetAnimParam)(GameCore::World& InWorld, GameCore::EntityID InEntity, const char* InParam,
                             float InValue, void* InUserData) = nullptr;
        void* UserData = nullptr;
    };

    /*
     * One brain's run of a tree: its blackboard and every node's memory.
     */
    class AICORE_API BTInstance
    {
    public:
        BTInstance(std::shared_ptr<const BehaviorTreeAsset> InAsset, GameCore::World& InWorld, GameCore::EntityID InSelf,
                   AIWorld* InAI = nullptr, uint32_t InSeed = 1);
        ~BTInstance();
        BTInstance(const BTInstance&)            = delete;
        BTInstance& operator=(const BTInstance&) = delete;

        /* One tick of the whole tree. The root's status; a finished root
         * starts again on the next tick. */
        EBTStatus Tick(float InDeltaSeconds);

        /* Every running node stopped (their OnExit, Aborted); starts from
         * the root on the next tick. */
        void Abort();

        Blackboard&       GetBlackboard()       { return Board; }
        const Blackboard& GetBlackboard() const { return Board; }
        const BehaviorTreeAsset& GetAsset() const { return *Asset; }
        const std::shared_ptr<const BehaviorTreeAsset>& GetAssetPtr() const { return Asset; }
        GameCore::EntityID GetSelf() const { return Self; }
        double             GetTime() const { return Clock; }
        uint64_t           GetTickCount() const { return Ticks; }
        EBTStatus          GetLastStatus() const { return LastStatus; }

        const BTNodeMemory& GetNodeMemory(uint32_t InNode) const { return Memory[InNode]; }
        bool IsNodeActive(uint32_t InNode) const { return InNode < Memory.size() && Memory[InNode].bActive != 0; }
        /* The active nodes, root first: what the debugger highlights. */
        void GetActivePath(std::vector<uint32_t>& OutNodes) const;
        /* A UtilitySelector child's last score; -1 if never scored. */
        float GetUtilityScore(uint32_t InNode) const;
        /* Seconds each node has spent active (the debugger, the arena's stats). */
        double GetNodeActiveSeconds(uint32_t InNode) const;

        /* For node code: the World, the AIWorld, a random number. */
        GameCore::World& GetWorld() { return World; }
        AIWorld*         GetAI() const { return AI; }
        float            NextRandom();

        // === For the runtime's composites (BTRuntime.cpp, UtilityNodes.cpp)
        EBTStatus TickNode(uint32_t InNode, float InDeltaSeconds);
        void      AbortNode(uint32_t InNode);
        BTContext MakeContext(uint32_t InNode, const std::vector<BTParam>& InParams, BTNodeMemory& InMemory, float InDeltaSeconds);
        /* Whether every condition decorator of InNode passes now. */
        bool      ConditionsPass(uint32_t InNode, float InDeltaSeconds);
        BTNodeMemory& GetDecoratorMemory(uint32_t InSlot) { return DecoratorMemory[InSlot]; }
        BTNodeMemory& GetServiceMemory(uint32_t InSlot)   { return ServiceMemory[InSlot]; }
        BTNodeMemory& GetMutableNodeMemory(uint32_t InNode) { return Memory[InNode]; }
        void          SetUtilityScore(uint32_t InNode, float InScore) { if(InNode < UtilityScores.size()) { UtilityScores[InNode] = InScore; } }
        double        GetClock() const { return Clock; }

    private:
        EBTStatus Enter(uint32_t InNode, float InDeltaSeconds);
        EBTStatus Finish(uint32_t InNode, EBTStatus InStatus, float InDeltaSeconds);
        EBTStatus TickComposite(uint32_t InNode, float InDeltaSeconds);
        EBTStatus TickSequence(uint32_t InNode, bool bInSelector, float InDeltaSeconds);
        EBTStatus TickParallel(uint32_t InNode, float InDeltaSeconds);
        EBTStatus TickRandomSelector(uint32_t InNode, float InDeltaSeconds);
        void      TickServices(uint32_t InNode, float InDeltaSeconds, bool bInEntering);
        /* A Selector's earlier child whose LowerPriority condition now passes. */
        uint32_t  FindLowerPriorityAbort(uint32_t InNode, float InDeltaSeconds);
        bool      SelfAbortFails(uint32_t InNode, float InDeltaSeconds);

        std::shared_ptr<const BehaviorTreeAsset> Asset;
        GameCore::World&          World;
        GameCore::EntityID        Self;
        AIWorld*                  AI = nullptr;
        Blackboard                Board;
        std::vector<BTNodeMemory> Memory;
        std::vector<BTNodeMemory> DecoratorMemory;
        std::vector<BTNodeMemory> ServiceMemory;
        std::vector<double>       ActiveSeconds;
        std::vector<float>        UtilityScores;
        double    Clock = 0.0;
        uint64_t  Ticks = 0;
        uint32_t  RandomState = 1;
        EBTStatus LastStatus = EBTStatus::Success;
    };

    // ========================================================================
    // The registry
    // ========================================================================

    /* Process-wide, by name (case-sensitive). The returned objects live for
     * the process (a game's are its module's statics: unregistered as it
     * unloads, by the macros below). */
    AICORE_API void RegisterBTTask(const char* InName, const BTTask* InTask, const char* InDisplayName = nullptr);
    AICORE_API void RegisterBTDecorator(const char* InName, const BTDecorator* InDecorator);
    AICORE_API void RegisterBTService(const char* InName, const BTService* InService);
    AICORE_API void UnregisterBTType(const void* InImpl);

    AICORE_API const BTTask*      FindBTTask(const char* InName);
    AICORE_API const BTDecorator* FindBTDecorator(const char* InName);
    AICORE_API const BTService*   FindBTService(const char* InName);
    /* The names registered, for an editor's palette. */
    AICORE_API std::vector<std::string> GetBTTaskNames();
    AICORE_API std::vector<std::string> GetBTDecoratorNames();
    AICORE_API std::vector<std::string> GetBTServiceNames();
    AICORE_API std::string GetBTTaskDisplayName(const char* InName);

    /* What the macros keep: registers on construction, unregisters on
     * destruction (the module's unload). */
    struct AICORE_API BTRegistration
    {
        enum EKind : uint8_t { Task, Decorator, Service };
        BTRegistration(EKind InKind, const char* InName, const void* InImpl, const char* InDisplayName = nullptr);
        ~BTRegistration();
        const void* Impl;
    };
}

/*
 * A task, decorator or service type, at file scope in one .cpp:
 *
 *     class AimAndFire : public AICore::BTTask { ... };
 *     AICORE_DEFINE_BT_TASK(AimAndFire, "Aim and Fire")
 *
 * registers it as "AimAndFire" for as long as its module is loaded.
 */
#define AICORE_DEFINE_BT_TASK(TypeName, DisplayLiteral)                                                         \
    namespace { const TypeName G##TypeName##_Instance{};                                                         \
                const ::AICore::BTRegistration G##TypeName##_Registration(                                      \
                    ::AICore::BTRegistration::Task, #TypeName, &G##TypeName##_Instance, DisplayLiteral); }

#define AICORE_DEFINE_BT_DECORATOR(TypeName)                                                                    \
    namespace { const TypeName G##TypeName##_Instance{};                                                         \
                const ::AICore::BTRegistration G##TypeName##_Registration(                                      \
                    ::AICore::BTRegistration::Decorator, #TypeName, &G##TypeName##_Instance); }

#define AICORE_DEFINE_BT_SERVICE(TypeName)                                                                      \
    namespace { const TypeName G##TypeName##_Instance{};                                                         \
                const ::AICore::BTRegistration G##TypeName##_Registration(                                      \
                    ::AICore::BTRegistration::Service, #TypeName, &G##TypeName##_Instance); }

#endif //AICORE_BEHAVIORTREE_H
