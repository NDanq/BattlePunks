#ifndef AICORE_BEHAVIORTREEFILE_H
#define AICORE_BEHAVIORTREEFILE_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "Public/AICore/AICoreAPI.h"
#include "Public/AICore/BehaviorTree.h"

/*
 * .nmbt files: XML as authored (the editor's, and readable in a diff),
 * binary as cooked (a game's) -- one extension, told apart by the first
 * bytes, as .nmseq's are. A Shipping build reads only the cooked form.
 *
 * THE XML.
 *
 *     <BehaviorTree Format="1" Name="ArenaBot">
 *       <Blackboard>
 *         <Key Name="TargetActor" Type="Entity" />
 *         <Key Name="Health" Type="Float" Default="1" />
 *       </Blackboard>
 *       <UtilitySelector Interval="0.25">
 *         <Sequence Name="Engage">
 *           <Consideration Input="TargetVisible" />
 *           <Consideration Input="distance(TargetActor)" Max="3000" Curve="InverseLinear" />
 *           <Decorator Type="BlackboardCondition" Key="TargetActor" Op="IsSet" Abort="Self" />
 *           <Task Type="MoveTo" Key="TargetActor" AcceptanceRadius="800" />
 *         </Sequence>
 *         ...
 *       </UtilitySelector>
 *     </BehaviorTree>
 *
 * The root element's one node child is the root. A node element is a
 * composite by its name (Sequence, Selector, Parallel, RandomSelector,
 * UtilitySelector) or a <Task Type="...">; its attributes are its params,
 * except Name and Weight. <Decorator>, <Service> and <Consideration>
 * children attach to it.
 *
 * THE BINARY. "NMBT", a version, then chunks (CoreArchive): KEYS the
 * blackboard, NODE the nodes. A chunk a newer build adds is stepped over.
 *
 * Every read returns a resolved asset (BehaviorTreeAsset::Resolve).
 */
namespace AICore
{
    enum class EBTFileFormat : uint8_t { Unknown, Text, Binary };
    AICORE_API EBTFileFormat DetectBehaviorTreeFormat(const std::vector<uint8_t>& InBytes);

    AICORE_API bool LoadBehaviorTree(const std::string& InPath, BehaviorTreeAsset& OutAsset, std::string* OutError = nullptr);
    AICORE_API bool ReadBehaviorTree(const std::vector<uint8_t>& InBytes, BehaviorTreeAsset& OutAsset, std::string* OutError = nullptr);

    /* The cooked form. Deterministic. */
    AICORE_API std::vector<uint8_t> WriteBehaviorTreeBinary(const BehaviorTreeAsset& InAsset);
    AICORE_API bool ReadBehaviorTreeBinary(const std::vector<uint8_t>& InBytes, BehaviorTreeAsset& OutAsset, std::string* OutError = nullptr);

#if WITH_UNCOOKED
    /* bInResolve false keeps a tree whose types this process has not
     * registered (an editor without the game's module): unresolved. */
    AICORE_API bool        ParseBehaviorTreeXml(const std::string& InText, BehaviorTreeAsset& OutAsset, std::string* OutError = nullptr,
                                                bool bInResolve = true);
    /* Deterministic: the same asset always writes the same bytes. */
    AICORE_API std::string WriteBehaviorTreeXml(const BehaviorTreeAsset& InAsset);
    AICORE_API bool        SaveBehaviorTreeXml(const std::string& InDiskPath, const BehaviorTreeAsset& InAsset, std::string* OutError = nullptr);
    /* Reads InSourceDiskPath and writes its cooked form to InOutDiskPath. */
    AICORE_API bool        CookBehaviorTreeFile(const std::string& InSourceDiskPath, const std::string& InOutDiskPath, std::string* OutError = nullptr);
#endif

    /*
     * Loaded trees by runtime path, shared by every brain. A failed load is
     * warned once and remembered until Invalidate. Game thread only.
     */
    class AICORE_API BehaviorTreeAssetCache
    {
    public:
        static std::shared_ptr<const BehaviorTreeAsset> Find(const std::string& InPath);
        /* Forget InPath (or everything, given ""): an editor's save. Running
         * brains keep the tree they have; AIWorld restarts them on the next
         * tick when the generation moved. */
        static void     Invalidate(const std::string& InPath);
        static uint64_t GetGeneration();
    };
}

#endif //AICORE_BEHAVIORTREEFILE_H
