#include "Public/NimbleEngine/GameSession.h"

#include <string>

#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/World.h"

/*
 * Components whose module this build compiled out: GameCore keeps them, so
 * levels and prefabs load, edit and save unchanged, but nothing acts on them.
 * Said once when a level (or the editor's world) has any, rather than leaving
 * a game to wonder why its crates float or its players are not replicated.
 */
namespace NimbleEngine
{
    namespace
    {
        size_t CountOf(const GameCore::World& InWorld, const GameCore::ComponentTypeID InType)
        {
            const GameCore::ComponentPool* Pool = InWorld.FindPool(InType);
            return Pool ? Pool->GetCount() : 0;
        }

        void Warn(const char* InContext, const size_t InCount, const char* InComponent, const char* InModule,
                  const char* InOption, const char* InEffect)
        {
            CORE_LOG_WARN(std::string("'") + (InContext ? InContext : "") + "' has " + std::to_string(InCount) + " "
                          + InComponent + " component(s), but " + InModule + " is not compiled in (" + InOption
                          + "=OFF); " + InEffect)
        }
    }

    bool WarnInactiveModuleComponents(const GameCore::World& InWorld, const char* InContext)
    {
        bool bWarned = false;
        (void)InWorld;
        (void)InContext;

#if !WITH_JOLTPHYSICS
        const size_t Bodies = CountOf(InWorld, GameCore::RigidBodyComponent::StaticType().ID);
        if(Bodies > 0)
        {
            Warn(InContext, Bodies, "Rigid Body", "JoltPhysics", "NIMBLE_WITH_JOLTPHYSICS", "they will not simulate");
            bWarned = true;
        }
        const size_t Joints = CountOf(InWorld, GameCore::PhysicsConstraintComponent::StaticType().ID);
        if(Joints > 0)
        {
            Warn(InContext, Joints, "Physics Constraint", "JoltPhysics", "NIMBLE_WITH_JOLTPHYSICS", "they will not act");
            bWarned = true;
        }
#endif

#if !WITH_NETCORE
        const size_t Networked = CountOf(InWorld, GameCore::NetComponent::StaticType().ID);
        if(Networked > 0)
        {
            Warn(InContext, Networked, "Net", "NetCore", "NIMBLE_WITH_NETCORE", "they will not replicate");
            bWarned = true;
        }
#endif

        return bWarned;
    }

    bool IsInactiveModuleComponent(const GameCore::ComponentTypeID InType)
    {
        (void)InType;
#if !WITH_JOLTPHYSICS
        if(InType == GameCore::RigidBodyComponent::StaticType().ID
           || InType == GameCore::PhysicsConstraintComponent::StaticType().ID) { return true; }
#endif
#if !WITH_NETCORE
        if(InType == GameCore::NetComponent::StaticType().ID) { return true; }
#endif
        return false;
    }
}
