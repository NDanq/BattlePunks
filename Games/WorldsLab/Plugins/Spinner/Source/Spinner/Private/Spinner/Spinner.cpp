#include "Public/Spinner/SpinnerComponent.h"

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Components/TransformComponent.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"

/* The whole of Spinner -- each piece of a plugin in one file:
 *
 *   1. a config var, whose default the plugin's Config/DefaultSpinner.ini sets
 *   2. a component, registered as the library loads
 *   3. a script library, registered the same way
 *   4. the system that turns spinners, attached to every level's World by
 *      the IPlugin's world hook */

// 1. Config ------------------------------------------------------------------

NIMBLE_CONFIG_FLOAT(GSpinnerSpeedScale, "Spinner", "SpeedScale", 1.0f, nullptr);

// 2. The component -----------------------------------------------------------

namespace Spinner
{
    GAMECORE_DEFINE_COMPONENT(SpinnerComponent, "Spinner")

    /* What a level file saves. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(SpinnerComponent, 1)
    {
        Ar.Field("DegreesPerSecond", C.DegreesPerSecond);
        Ar.Field("Axis",             C.Axis);
        Ar.Flag("Running",           C.bRunning);
    }

    /* What Lua and script graphs see on the component. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(SpinnerComponent)
    {
        Bind.Field("DegreesPerSecond", &SpinnerComponent::DegreesPerSecond);
        Bind.Flag("Running",           &SpinnerComponent::bRunning);
    }

#if WITH_EDITOR
    /* The editor's Details panel and Add Component picker. */
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(SpinnerComponent)
    {
        Props.TypeCategory("Gameplay");
        Props.Category("Spinner");
        Props.Field("Degrees Per Second", &SpinnerComponent::DegreesPerSecond);
        Props.Range(-720.0f, 720.0f);
        Props.Field("Axis", &SpinnerComponent::Axis);
        Props.Range(0.0f, 2.0f);
        Props.Tooltip("0 X, 1 Y, 2 Z");
        Props.Flag("Running", &SpinnerComponent::bRunning);
    }
#endif

    void Tick(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        const float Scale = GSpinnerSpeedScale.GetFloat();
        InWorld.Each<SpinnerComponent>([&](GameCore::Entity InEntity, SpinnerComponent& Spin)
        {
            GameCore::TransformComponent* Transform = InEntity.GetComponent<GameCore::TransformComponent>();
            if(!Spin.bRunning || !Transform) { return; }
            EngineCore::Vector3 Rotation = Transform->Rotation;
            const float Step = Spin.DegreesPerSecond * Scale * InDeltaSeconds;
            if(Spin.Axis == 0)      { Rotation.SetX(Rotation.GetX() + Step); }
            else if(Spin.Axis == 2) { Rotation.SetZ(Rotation.GetZ() + Step); }
            else                    { Rotation.SetY(Rotation.GetY() + Step); }
            Transform->Rotation = Rotation;
        });
    }
}

// 3. The script library: Spinner.SetSpeed(entity, dps), Spinner.GetSpeed(entity)

namespace
{
    Spinner::SpinnerComponent* Find(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        if(InCall.HasError()) { return nullptr; }
        auto* Spin = InCall.GetWorld().GetEntity(Entity).GetComponent<Spinner::SpinnerComponent>();
        if(!Spin) { InCall.Error("the entity has no Spinner component"); }
        return Spin;
    }

    void ScriptSetSpeed(GameCore::ScriptCall& InCall)
    {
        const double Speed = InCall.GetNumber(2);
        if(Spinner::SpinnerComponent* Spin = Find(InCall))
        {
            Spin->DegreesPerSecond = float(Speed);
        }
    }

    void ScriptGetSpeed(GameCore::ScriptCall& InCall)
    {
        if(Spinner::SpinnerComponent* Spin = Find(InCall))
        {
            InCall.PushNumber(Spin->DegreesPerSecond);
        }
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(Spinner)
{
    Lib.Function("SetSpeed", &ScriptSetSpeed, "Spinner:Entity, DegreesPerSecond:number");
    Lib.Function("GetSpeed", &ScriptGetSpeed, "Spinner:Entity -> DegreesPerSecond:number");
}

// 4. The plugin: its system on every level's World ---------------------------

namespace
{
    bool GStarted = false;
    char GSystemOwner = 0;   // its address keys this plugin's systems on a World

    void TickSpinners(GameCore::World& InWorld, const float InDeltaSeconds, void*)
    {
        Spinner::Tick(InWorld, InDeltaSeconds);
    }

    class SpinnerPlugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            NimbleEngine::PluginWorldHook Hook;
            Hook.Name   = "Spinner";
            Hook.Attach = [](GameCore::World& InWorld, void*)
            {
                InWorld.RegisterSystem("Spinner", GameCore::TP_PostPhysics, &TickSpinners, &GSystemOwner);
            };
            Hook.Detach = [](GameCore::World& InWorld, void*) { InWorld.UnregisterAllSystems(&GSystemOwner); };
            InContext.AddWorldHook(Hook);
            GStarted = true;
            return true;
        }

        void OnShutdown(NimbleEngine::PluginContext&) override
        {
            GStarted = false;
        }
    };
}

namespace Spinner
{
    bool IsStarted() { return GStarted; }
}

NIMBLE_IMPLEMENT_PLUGIN(SpinnerPlugin)
