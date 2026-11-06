#include "Public/@PLUGIN_NAME@/@PLUGIN_NAME@.h"

#include <map>
#include <memory>

#include "Public/Core/CoreConfig.h"
#include "Public/GameCore/World.h"
#include "Public/NimbleEngine/Plugin.h"
#include "Public/RenderCore/CustomRenderPass.h"

/* A custom render pass from a plugin (Engine/Docs/Guides/CustomRenderPassGuide.md
 * for the pass itself; PluginGuide.md, "Shaders and render passes", for the
 * plugin's part): registered on every level's World by the IPlugin's world
 * hook, drawing Content/Shaders/@PLUGIN_NAME@Tint.fsh over the finished picture. */

NIMBLE_CONFIG_FLOAT(G@PLUGIN_NAME@Strength, "@PLUGIN_NAME@", "Strength", 0.35f, nullptr);

namespace
{
    struct TintSettings
    {
        float Strength = 0.0f;
    };

    class TintPass final : public RenderCore::CustomRenderPass
    {
    public:
        const char* GetName() const override { return "@PLUGIN_NAME@"; }

        /* Game thread, once a frame: hand the render thread this frame's values. */
        void Extract() override
        {
            TintSettings Now;
            Now.Strength = G@PLUGIN_NAME@Strength.GetFloat();
            Settings.Put(Now);
        }

        /* Render thread. */
        void Execute(RenderCore::RenderPassContext& InContext) override
        {
            using namespace RenderCore;
            const TintSettings Now = Settings.Get();
            if(Now.Strength <= 0.0f)
            {
                return;
            }
            const RenderPassTarget Out = InContext.AcquireTarget("Picture", RenderPassTargetDesc());   // the view's size
            RenderPassParams Params;
            Params.A[0] = Now.Strength;
            if(InContext.DrawFullscreen("@PLUGIN_NAME@Tint.fsh", Out, { InContext.GetSceneTexture(ESceneTexture::Current) }, Params))
            {
                InContext.SetCurrent(Out);   // the picture from here on: the UI draws over it
            }
        }

    private:
        RenderCore::RenderPassMailbox<TintSettings> Settings;
    };

    /* Game thread only, as World hooks are. */
    std::map<const GameCore::World*, RenderCore::RenderPassHandle>& Passes()
    {
        static std::map<const GameCore::World*, RenderCore::RenderPassHandle> Handles;
        return Handles;
    }

    bool GStarted = false;

    class @PLUGIN_NAME@Plugin : public NimbleEngine::IPlugin
    {
    public:
        bool OnStartup(NimbleEngine::PluginContext& InContext) override
        {
            NimbleEngine::PluginWorldHook Hook;
            Hook.Name   = "@PLUGIN_NAME@";
            Hook.Attach = [](GameCore::World& InWorld, void*) { @PLUGIN_NAME@::AttachPass(InWorld); };
            Hook.Detach = [](GameCore::World& InWorld, void*) { @PLUGIN_NAME@::DetachPass(InWorld); };
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
    void AttachPass(GameCore::World& InWorld)
    {
        if(Passes().count(&InWorld) != 0)
        {
            return;
        }
        const RenderCore::RenderPassHandle Handle =
            RenderCore::RegisterCustomRenderPass(&InWorld, std::make_shared<TintPass>(), RenderCore::ERenderHook::AfterPostProcess);
        if(Handle != 0)
        {
            Passes()[&InWorld] = Handle;
        }
    }

    void DetachPass(GameCore::World& InWorld)
    {
        const auto Found = Passes().find(&InWorld);
        if(Found != Passes().end())
        {
            RenderCore::UnregisterCustomRenderPass(Found->second);
            Passes().erase(Found);
        }
    }

    bool IsStarted() { return GStarted; }
}

/* Exactly one per library, at file scope, outside any namespace. */
NIMBLE_IMPLEMENT_PLUGIN(@PLUGIN_NAME@Plugin)
