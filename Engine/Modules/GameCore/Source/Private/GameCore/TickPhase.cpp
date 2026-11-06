#include "Public/GameCore/TickPhase.h"

namespace GameCore
{
    const char* TickPhaseName(const ETickPhase InPhase)
    {
        switch(InPhase)
        {
            case TP_PrePhysics:  return "PrePhysics";
            case TP_Physics:     return "Physics";
            case TP_PostPhysics: return "PostPhysics";
            case TP_PostUpdate:  return "PostUpdate";
            default:             return "Unknown";
        }
    }
}
