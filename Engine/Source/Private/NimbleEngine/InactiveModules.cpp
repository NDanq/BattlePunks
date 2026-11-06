#include "Public/NimbleEngine/GameSession.h"

#include <string>

#include "Public/Core/CoreConfig.h"
#include "Public/Core/CoreLog.h"
#include "Public/GameCore/ComponentPool.h"
#include "Public/GameCore/Components/AudioListenerComponent.h"
#include "Public/GameCore/Components/AudioSourceComponent.h"
#include "Public/GameCore/Components/NetComponent.h"
#include "Public/GameCore/Components/PhysicsConstraintComponent.h"
#include "Public/GameCore/Components/RigidBodyComponent.h"
#include "Public/GameCore/Components/VehicleComponent.h"
#include "Public/GameCore/World.h"

/*
 * [Modules]: which optional engine modules the project builds with. CMake
 * reads them from the project's DefaultEngine.ini as the game configures
 * (Engine/cmake/NimbleProject.cmake) -- these vars are how the file's keys are
 * known at runtime, how Project Settings shows them as switches, and how a
 * build that has not yet caught up with an edit says so. Defaulted to what
 * this build has, so a file without the keys never disagrees with it.
 */
NIMBLE_CONFIG_BOOL(GModulesNetCore, "Modules", "NetCore", WITH_NETCORE != 0, nullptr);
NIMBLE_CONFIG_BOOL(GModulesJoltPhysics, "Modules", "JoltPhysics", WITH_JOLTPHYSICS != 0, nullptr);
NIMBLE_CONFIG_BOOL(GModulesAudio, "Modules", "Audio", WITH_AUDIO != 0, nullptr);

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
        const size_t Vehicles = CountOf(InWorld, GameCore::VehicleComponent::StaticType().ID);
        if(Vehicles > 0)
        {
            Warn(InContext, Vehicles, "Vehicle", "JoltPhysics", "NIMBLE_WITH_JOLTPHYSICS", "they will not drive");
            bWarned = true;
        }
#endif

#if !WITH_AUDIO
        const size_t Sources = CountOf(InWorld, GameCore::AudioSourceComponent::StaticType().ID);
        if(Sources > 0)
        {
            Warn(InContext, Sources, "Audio Source", "Audio", "NIMBLE_WITH_AUDIO", "they will be silent");
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

    bool WarnModuleConfigMismatch()
    {
        bool bWarned = false;
        const auto Check = [&bWarned](const bool bInWanted, const bool bInBuilt, const char* InKey, const char* InOption)
        {
            if(bInWanted == bInBuilt) { return; }
            CORE_LOG_WARN(std::string("[Modules] ") + InKey + "=" + (bInWanted ? "true" : "false") + ", but this build was configured "
                          + (bInBuilt ? "with" : "without") + " it (" + InOption + "=" + (bInBuilt ? "ON" : "OFF")
                          + "); rebuild the game to apply the change -- its next build re-configures from the project's config")
            bWarned = true;
        };
        Check(GModulesNetCore.GetBool(), WITH_NETCORE != 0, "NetCore", "NIMBLE_WITH_NETCORE");
        Check(GModulesJoltPhysics.GetBool(), WITH_JOLTPHYSICS != 0, "JoltPhysics", "NIMBLE_WITH_JOLTPHYSICS");
        Check(GModulesAudio.GetBool(), WITH_AUDIO != 0, "Audio", "NIMBLE_WITH_AUDIO");
        return bWarned;
    }

    bool IsInactiveModuleComponent(const GameCore::ComponentTypeID InType)
    {
        (void)InType;
#if !WITH_JOLTPHYSICS
        if(InType == GameCore::RigidBodyComponent::StaticType().ID
           || InType == GameCore::PhysicsConstraintComponent::StaticType().ID
           || InType == GameCore::VehicleComponent::StaticType().ID) { return true; }
#endif
#if !WITH_NETCORE
        if(InType == GameCore::NetComponent::StaticType().ID) { return true; }
#endif
#if !WITH_AUDIO
        if(InType == GameCore::AudioSourceComponent::StaticType().ID
           || InType == GameCore::AudioListenerComponent::StaticType().ID) { return true; }
#endif
        return false;
    }
}
