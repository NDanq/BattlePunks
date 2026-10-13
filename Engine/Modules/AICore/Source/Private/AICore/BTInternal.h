#ifndef AICORE_BTINTERNAL_H
#define AICORE_BTINTERNAL_H

#include <cstdint>

#include "Public/AICore/BehaviorTree.h"

/* What the runtime's sources share: the UtilitySelector lives in
 * UtilityNodes.cpp, beside the curves it scores with. */
namespace AICore
{
    namespace BTDetail
    {
        EBTStatus TickUtilitySelector(BTInstance& InInstance, uint32_t InNode, float InDeltaSeconds);

        /* A world position for a blackboard key: an entity's, or a vector.
         * False when the key is unset or of another type. */
        bool ResolveKeyPosition(BTInstance& InInstance, int32_t InKey, EngineCore::Vector3& OutPosition);

        /* The brain's own position (its entity's world position). */
        EngineCore::Vector3 SelfPosition(BTInstance& InInstance);
    }
}

#endif //AICORE_BTINTERNAL_H
