#ifndef RENDERCORE_CUSTOMRENDERPASS_H
#define RENDERCORE_CUSTOMRENDERPASS_H

#include <cstdint>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "Public/RenderCore/RenderCoreAPI.h"

/*
 * CUSTOM RENDER PASSES (RENDERPIPELINE.md, "Custom render passes"): GPU work
 * a game or engine module adds to a World's frame at a named point between
 * the built-in stages, without touching GDI -- no GDI type appears here, as
 * in RenderContext.h.
 *
 *   class OutlinePass final : public RenderCore::CustomRenderPass
 *   {
 *       const char* GetName() const override { return "Outline"; }
 *       void Execute(RenderCore::RenderPassContext& InContext) override
 *       {
 *           RenderCore::RenderPassTarget Mask = InContext.AcquireTarget("Mask", {});
 *           InContext.DrawMeshGroup(0, "OutlineMask.fsh", Mask, RenderCore::ERenderPassDepth::Scene, true);
 *           RenderCore::RenderPassParams Params;
 *           InContext.DrawFullscreen("OutlineComposite.fsh", {}, { InContext.AsTexture(Mask) }, Params,
 *                                    RenderCore::ERenderPassBlend::AlphaBlend);
 *       }
 *   };
 *   RenderPassHandle Handle = RegisterCustomRenderPass(&World, std::make_shared<OutlinePass>(), ERenderHook::AfterForward);
 *   ...
 *   UnregisterCustomRenderPass(Handle);   // before the World goes
 *
 * THREADS. Register and unregister on the game thread; Extract runs there
 * once a frame (WorldBridge, before the frame is handed over); ShouldRun and
 * Execute run on the render thread, which may still be drawing the frame
 * before. What Extract hands Execute must cross safely: RenderPassMailbox
 * below is the simple way.
 *
 * SHADERS are cooked like the engine's own (.fsh and .csh files in Content/Shaders,
 * cooked by NimbleCook into the module's shader cache) and named by file:
 * "GrassTrail.fsh". A full-screen pixel shader includes PostProcessCommon.glsl
 * and declares its PP_Block at binding 2 as RenderPassParams lays it out
 * (mat4, vec4, vec4); its inputs are material textures 0-6 (bindings 4-10),
 * slot 7 the scene depth. A compute shader includes ComputeCommon.glsl.
 */
namespace RenderCore
{
    struct RenderView;

    /* Where in the frame a pass runs (RENDERPIPELINE.md's stage table). */
    enum class ERenderHook : uint8_t
    {
        BeforeScene = 0,     // after the background, before the PrePass: what this frame's materials sample
        AfterPrePass,        // occluders' depth only
        AfterBasePass,       // the G-buffer, velocity and depth are written (a deferred frame); before AO and Shadows
        AfterLighting,       // the lit scene colour (linear), before the forward stages
        AfterForward,        // every surface drawn, before volumetric fog
        BeforePostProcess,   // the final linear scene colour, before TAA and the tone map
        AfterToneMap,        // display space, before the post process stack; SetCurrent replaces it
        AfterPostProcess,    // the finished picture, before the UI; SetCurrent replaces it
        Count
    };

    RENDERCORE_API const char* GetRenderHookName(ERenderHook InHook);

    /* The scene's own textures a pass may read; null where this frame has none. */
    enum class ESceneTexture : uint8_t
    {
        SceneColor = 0,      // linear HDR where the backend can
        SceneDepth,          // window depth, 0 near
        GBuffer0,            // base colour, AO (the screen's folded in once the Lighting stage runs)
        GBuffer1,            // normal, octahedral (Common.glsl's DecodeGBufferNormal)
        GBuffer2,            // metallic, roughness, specular, shading model
        GBuffer3,            // emissive
        GBuffer4,            // the shading model's custom data (ShadingModels.glsl)
        Velocity,
        Current,             // the post hooks: the picture so far (else the scene colour)
        Count
    };

    /* A pass target's format. RGBA32F and R32F (sampled as (r, 0, 0, 1)) are for values
     * half floats round away -- twice RGBA16F's bandwidth, so not for colour. Where the
     * backend lacks one, a target is RGBA16F, else RGBA8, with a warning once. */
    enum class ERenderPassFormat : uint8_t { RGBA8 = 0, RGBA16F, RGBA32F, R32F };
    enum class ERenderPassBlend  : uint8_t { Opaque = 0, Additive, AlphaBlend };

    /* DrawMeshGroup's depth: none, the target's own, or the scene's (same size). */
    enum class ERenderPassDepth  : uint8_t { None = 0, Own, Scene };

    enum : uint32_t
    {
        RENDER_PASS_INPUTS_MAX         = 7,      // DrawFullscreen's textures; slot 7 is the scene depth
        RENDER_PASS_MESH_GROUPS        = 8,      // RenderComponent::CustomPassGroups' bits
        RENDER_PASS_BUFFERS_MAX        = 4,      // Dispatch's storage buffers
        RENDER_PASS_COMPUTE_PARAMS_MAX = 1024,   // Dispatch's parameter bytes
    };

    /* Handles, valid only during the Execute that made them (a pooled target's
     * or buffer's handle is the same object frame to frame, but ask again). */
    struct RenderPassTexture
    {
        const void* Handle = nullptr;
        bool IsValid() const { return Handle != nullptr; }
    };
    struct RenderPassTarget
    {
        void* Handle = nullptr;
        bool IsValid() const { return Handle != nullptr; }
    };
    struct RenderPassBuffer
    {
        void* Handle = nullptr;
        bool IsValid() const { return Handle != nullptr; }
    };

    /* A pass's own target: Width x Height, or (0) the view's size times Scale. */
    struct RenderPassTargetDesc
    {
        uint32_t          Width  = 0;
        uint32_t          Height = 0;
        float             Scale  = 1.0f;
        ERenderPassFormat Format = ERenderPassFormat::RGBA8;
    };

    /* A full-screen pass's PP_Block (binding 2), std140: mat4 Matrix; vec4 A; vec4 B. */
    struct RenderPassParams
    {
        float Matrix[16] = {};
        float A[4]       = {};
        float B[4]       = {};
    };

    /*
     * What a pass draws with, inside Execute only. Every draw binds its own
     * target and leaves the stages after it to bind theirs.
     */
    class RENDERCORE_API RenderPassContext
    {
    public:
        struct Frame;   // RenderContext.cpp
        explicit RenderPassContext(Frame& InFrame) : F(&InFrame) {}

        RenderPassContext(const RenderPassContext&)            = delete;
        RenderPassContext& operator=(const RenderPassContext&) = delete;

        ERenderHook       GetHook() const;
        const RenderView& GetView() const;      // as drawn (TAA's jitter included)
        uint32_t          GetWidth() const;     // the scene's targets
        uint32_t          GetHeight() const;
        bool              IsMainView() const;   // a World's own view, not a scene capture's
        uint32_t          GetViewIndex() const; // which of its scene's views (splitscreen: 0 to RENDER_VIEWS_MAX - 1)
        bool              IsHDR() const;        // the scene colour is linear half float
        uint64_t          GetFrameNumber() const;
        float             GetDeltaSeconds() const;

        RenderPassTexture GetSceneTexture(ESceneTexture InTexture) const;

        /* This pass's target named InName, made on first use and kept (with
         * what it holds) while the size and format stay; resized otherwise. */
        RenderPassTarget  AcquireTarget(const char* InName, const RenderPassTargetDesc& InDesc);

        /* A render target asset (.nmrt2d, or a transient one) to draw into,
         * at its own size: what every material naming it samples after this.
         * Invalid (warned once) if InPath names none. */
        RenderPassTarget  AcquireAssetTarget(const char* InPath);

        RenderPassTexture AsTexture(RenderPassTarget InTarget) const;
        RenderPassTexture AsDepthTexture(RenderPassTarget InTarget) const;   // its depth, window depth

        /* InTarget's colour (and its depth to 1, with bInDepth) cleared. */
        bool Clear(RenderPassTarget InTarget, const float (&InColor)[4], bool bInDepth = false);

        /* InPixelShader over the whole of InTarget (invalid: the scene colour),
         * InInputs as material textures 0.., InParams as PP_Block. False if
         * the shader or target is missing (warned once). */
        bool DrawFullscreen(const char* InPixelShader, RenderPassTarget InTarget, std::initializer_list<RenderPassTexture> InInputs,
                            const RenderPassParams& InParams, ERenderPassBlend InBlend = ERenderPassBlend::Opaque);

        /* The mesh draws queued this frame by entities in group InGroup
         * (RenderComponent::CustomPassGroups' bit InGroup), seen from the
         * view, into InTarget: each with its material's vertex shader (World
         * Position Offset kept) and InPixelShader -- which sees the material's
         * block and the scene block -- or, null, its own material. False if
         * the group drew nothing or something is missing. */
        bool DrawMeshGroup(uint32_t InGroup, const char* InPixelShader, RenderPassTarget InTarget,
                           ERenderPassDepth InDepth, bool bInClear);
        uint32_t GetMeshGroupDrawCount(uint32_t InGroup) const;

        /* COMPUTE, where the backend has it. A buffer is this pass's, by name,
         * like its targets: made (InBytes, from InInitialData or zeros) on
         * first use, remade if InBytes changes. bInInstances: an instanced
         * draw may also read it. */
        bool             SupportsCompute() const;
        RenderPassBuffer AcquireBuffer(const char* InName, uint32_t InBytes, const void* InInitialData = nullptr,
                                       bool bInInstances = false);
        bool Dispatch(const char* InComputeShader, const void* InParams, uint32_t InParamBytes,
                      std::initializer_list<RenderPassBuffer> InBuffers, uint32_t InX, uint32_t InY = 1, uint32_t InZ = 1);
        /* InBuffer's bytes as the GPU leaves them: stalls the frame -- now and then, for tools and tests. */
        bool ReadBuffer(RenderPassBuffer InBuffer, std::vector<uint8_t>& OutBytes);

        /* AfterToneMap, AfterPostProcess: InTarget (the scene's size) is the
         * picture from here on. Ignored at the other hooks. */
        void SetCurrent(RenderPassTarget InTarget);

        void BeginDrawEvent(const char* InName);
        void EndDrawEvent();

    private:
        Frame* F;
    };

    /*
     * A pass. Subclass it, register an instance for a World. Its targets and
     * buffers are released when it is unregistered.
     */
    class RENDERCORE_API CustomRenderPass
    {
    public:
        CustomRenderPass() = default;
        virtual ~CustomRenderPass() = default;

        CustomRenderPass(const CustomRenderPass&)            = delete;
        CustomRenderPass& operator=(const CustomRenderPass&) = delete;

        /* Its draw event ("Custom:<Name>") and its targets' names. */
        virtual const char* GetName() const = 0;

        /* GAME THREAD, once a frame, before the frame goes to the render thread. */
        virtual void Extract() {}

        /* RENDER THREAD: whether to run for this view. The main view only by default. */
        virtual bool ShouldRun(const RenderPassContext& InContext) const;

        /* RENDER THREAD. */
        virtual void Execute(RenderPassContext& InContext) = 0;
    };

    /* A registration; 0 is none. */
    using RenderPassHandle = uint64_t;

    /* InPass at InHook in InWorld's frames (a GameCore::World's address, as
     * SetPostProcessOverride keys it), after the passes there of lower InOrder
     * and, at equal order, those registered before it. 0 for a null pass. */
    RENDERCORE_API RenderPassHandle RegisterCustomRenderPass(const void* InWorld, std::shared_ptr<CustomRenderPass> InPass,
                                                             ERenderHook InHook, int32_t InOrder = 0);

    /* Stops it; its GPU resources are freed on the render thread. False if unknown. */
    RENDERCORE_API bool UnregisterCustomRenderPass(RenderPassHandle InHandle);

    /* Every pass of InWorld's. */
    RENDERCORE_API void UnregisterCustomRenderPasses(const void* InWorld);

    /* How many passes InWorld has (any thread). */
    RENDERCORE_API size_t GetCustomRenderPassCount(const void* InWorld);

    /*
     * Extract's values to Execute: Put on the game thread, Get on the render
     * thread -- the newest put, or what it had. Header-only.
     */
    template<typename T>
    class RenderPassMailbox
    {
    public:
        void Put(const T& InValue)
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            Value = InValue;
        }
        T Get() const
        {
            std::lock_guard<std::mutex> Lock(Mutex);
            return Value;
        }

    private:
        mutable std::mutex Mutex;
        T                  Value{};
    };
}

#endif //RENDERCORE_CUSTOMRENDERPASS_H
