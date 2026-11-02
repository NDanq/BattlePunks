#include "Public/@PLUGIN_NAME@/@PLUGIN_NAME@Component.h"

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/ComponentScript.h"
#if WITH_EDITOR
#include "Public/GameCore/ComponentProperties.h"
#endif
#include "Public/GameCore/ComponentSerializer.h"
#include "Public/GameCore/Entity.h"
#include "Public/GameCore/ScriptLibrary.h"
#include "Public/GameCore/TickPhase.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"

/* Each piece of a gameplay plugin, in one file:
 *
 *   1. a config var, whose default Config/Default@PLUGIN_NAME@.ini sets
 *   2. a component, registered as the library loads
 *   3. a script library, registered the same way
 *   4. the system that steps the components, on every level's World through
 *      the IPlugin's world hook */

// 1. Config ------------------------------------------------------------------

NIMBLE_CONFIG_FLOAT(G@PLUGIN_NAME@RateScale, "@PLUGIN_NAME@", "RateScale", 1.0f, nullptr);

// 2. The component -----------------------------------------------------------

namespace @PLUGIN_NAME@
{
    GAMECORE_DEFINE_COMPONENT(@PLUGIN_NAME@Component, "@PLUGIN_NAME@")

    /* What a level file saves. Bump the version when the fields change. */
    GAMECORE_DEFINE_COMPONENT_SERIALIZER(@PLUGIN_NAME@Component, 1)
    {
        Ar.Field("Rate",    C.Rate);
        Ar.Flag("Enabled",  C.bEnabled);
    }

    /* What Lua and script graphs see on the component. */
    GAMECORE_DEFINE_COMPONENT_SCRIPT(@PLUGIN_NAME@Component)
    {
        Bind.Field("Rate",  &@PLUGIN_NAME@Component::Rate);
        Bind.Field("Total", &@PLUGIN_NAME@Component::Total);
        Bind.Flag("Enabled", &@PLUGIN_NAME@Component::bEnabled);
    }

#if WITH_EDITOR
    /* The editor's Details panel and Add Component picker. */
    GAMECORE_DEFINE_COMPONENT_PROPERTIES(@PLUGIN_NAME@Component)
    {
        Props.TypeCategory("@PLUGIN_CATEGORY@");
        Props.Category("@PLUGIN_NAME@");
        Props.Field("Rate", &@PLUGIN_NAME@Component::Rate);
        Props.Range(-100.0f, 100.0f);
        Props.Tooltip("How fast Total grows, per second.");
        Props.Flag("Enabled", &@PLUGIN_NAME@Component::bEnabled);
    }
#endif

    void Tick(GameCore::World& InWorld, const float InDeltaSeconds)
    {
        const float Scale = G@PLUGIN_NAME@RateScale.GetFloat();
        InWorld.Each<@PLUGIN_NAME@Component>([&](GameCore::Entity, @PLUGIN_NAME@Component& InComponent)
        {
            if(InComponent.bEnabled)
            {
                InComponent.Total += InComponent.Rate * Scale * InDeltaSeconds;
            }
        });
    }
}

// 3. The script library: @PLUGIN_NAME@.GetTotal(entity), @PLUGIN_NAME@.SetRate(entity, rate)

namespace
{
    @PLUGIN_NAME@::@PLUGIN_NAME@Component* Find(GameCore::ScriptCall& InCall)
    {
        const GameCore::EntityID Entity = InCall.GetEntity(1);
        if(InCall.HasError()) { return nullptr; }
        auto* Component = InCall.GetWorld().GetEntity(Entity).GetComponent<@PLUGIN_NAME@::@PLUGIN_NAME@Component>();
        if(!Component) { InCall.Error("the entity has no @PLUGIN_NAME@ component"); }
        return Component;
    }

    void ScriptGetTotal(GameCore::ScriptCall& InCall)
    {
        if(@PLUGIN_NAME@::@PLUGIN_NAME@Component* Component = Find(InCall))
        {
            InCall.PushNumber(Component->Total);
        }
    }

    void ScriptSetRate(GameCore::ScriptCall& InCall)
    {
        const double Rate = InCall.GetNumber(2);
        if(@PLUGIN_NAME@::@PLUGIN_NAME@Component* Component = Find(InCall))
        {
            Component->Rate = float(Rate);
        }
    }
}

GAMECORE_DEFINE_SCRIPT_LIBRARY(@PLUGIN_NAME@)
{
    Lib.Function("GetTotal", &ScriptGetTotal, "Entity:Entity -> Total:number");
    Lib.Function("SetRate",  &ScriptSetRate,  "Entity:Entity, Rate:number");
}

// 4. The plugin: its system on every level's World ---------------------------

namespace
{
    bool GStarted = false;
    char GSystemOwner = 0;   // its address keys this plugin's systems on a World

    void TickAll(GameCore::World& InWorld, const float InDeltaSeconds, void*)
    {
        @PLUGIN_NAME@::Tick(InWorld, InDeltaSeconds);
    }

    class @PLUGIN_NAME@Plugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            NimbleEngine::PluginWorldHook Hook;
            Hook.Name   = "@PLUGIN_NAME@";
            Hook.Attach = [](GameCore::World& InWorld, void*)
            {
                InWorld.RegisterSystem("@PLUGIN_NAME@", GameCore::TP_PostPhysics, &TickAll, &GSystemOwner);
            };
            Hook.Detach = [](GameCore::World& InWorld, void*) { InWorld.UnregisterAllSystems(&GSystemOwner); };
            InContext.AddWorldHook(Hook);   // removed by the host after OnShutdown
            GStarted = true;
            return true;
        }

        void OnShutdown(NimbleEngine::PluginContext&) override
        {
            GStarted = false;
        }
    };
}

namespace @PLUGIN_NAME@
{
    bool IsStarted() { return GStarted; }
}

/* Exactly one per library, at file scope, outside any namespace. */
NIMBLE_IMPLEMENT_PLUGIN(@PLUGIN_NAME@Plugin)
