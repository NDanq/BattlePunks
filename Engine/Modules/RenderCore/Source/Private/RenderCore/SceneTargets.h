#ifndef RENDERCORE_SCENETARGETS_H
#define RENDERCORE_SCENETARGETS_H

#include <string>
#include <utility>

#include "Private/RenderCore/RenderCoreInternal.h"

/*
 * What a scene draws into (RENDERPIPELINE.md): the scene colour with the
 * scene depth, the G-buffer beside it, and a second colour target for
 * PostProcess to ping-pong with. The viewport's (RenderScene) and a material
 * preview's (RenderContext) are separate sets. The G-buffer and the
 * PostProcess target are created on first use and resized with the rest.
 * Render thread only.
 */
namespace RenderCore
{
    enum : uint32_t { RENDER_GBUFFER_TARGETS = 5 };

    /* A scene's image-based lighting targets (RENDERPIPELINE.md, "Image-based
     * lighting"), made by RenderContext::EnsureIBLResources: the probe atlas
     * (RGBA16F, IBLCommon.glsl's layout) and the cube strip a capture or the
     * sky is prefiltered from. The scene owns them; ReleaseIBLResources frees them. */
    /* The atlas: 20 blocks of 384 x 256, four to a row -- 0-15 the probes,
     * 16 the sky, 17 the sheen table (IBLCommon.glsl). */
    constexpr uint32_t IBL_ATLAS_WIDTH  = 1536;
    constexpr uint32_t IBL_ATLAS_HEIGHT = 1280;
    constexpr uint32_t IBL_SKY_BLOCK    = 16;
    constexpr uint32_t IBL_SHEEN_BLOCK  = 17;

    struct IBLResources
    {
        GDIRenderTarget* Atlas     = nullptr;
        GDIRenderTarget* Strip     = nullptr;
        uint32_t         StripFace = 0;
        bool             bSheenTable = false;   // the atlas's block 17 holds Cloth's sheen table (SHADING_MODELS.md)
    };

    /* A GI Volume's probes on the GPU (DDGI.glsl; GLOBAL_ILLUMINATION.md, Phase 6),
     * made by RenderContext::EnsureDDGIResources: the irradiance atlas (8 x 8 a
     * probe, RGBA16F), the distance atlas (16 x 16, RGBA32F where the backend has
     * it: its moments' variance, E[d^2] - E[d]^2, loses most of a half
     * float's bits to cancellation and the probes leak), tiles 64 to a row; and the
     * cube strip a probe's faces are rendered into (6 x 16 x 16, rgb radiance,
     * a distance). The scene owns them; ReleaseDDGIResources frees them. */
    constexpr uint32_t DDGI_TILES_PER_ROW   = 64;
    constexpr uint32_t DDGI_IRRADIANCE_TILE = 8;
    constexpr uint32_t DDGI_DISTANCE_TILE   = 16;
    constexpr uint32_t DDGI_FACE            = 16;
    struct DDGIResources
    {
        GDIRenderTarget* Irradiance = nullptr;
        GDIRenderTarget* Distance   = nullptr;
        GDIRenderTarget* Strip      = nullptr;
        uint32_t         Rows       = 0;

        /* RELIGHTING (GLOBAL_ILLUMINATION.md, "Relighting"; EnsureDDGICache): what each probe's
         * faces saw (DDGIStore.fsh: Cache[0] albedo and distance, RGBA16F; Cache[1] emissive and
         * normal, RGBA32F) and their radiance lit again from it (DDGIRelight.fsh, RGBA16F) --
         * probe i's six faces side by side in its row, CachePerColumn probes a column. */
        GDIRenderTarget* Cache[2]       = {};
        GDIRenderTarget* Radiance       = nullptr;
        uint32_t         CachePerColumn = 0;
    };
    constexpr uint32_t DDGI_CACHE_PER_COLUMN = 1024;   // a column of the cache: 16384 texels high

    struct SceneTargets
    {
        std::string      Name;                                  // a prefix for the targets' debug names
        GDIRenderTarget* SceneColor  = nullptr;
        bool             bHDR        = false;                   // SceneColor is RGBA16F, linear light (RENDERPIPELINE.md, "HDR and tone mapping")
        GDIRenderTarget* GBuffer[RENDER_GBUFFER_TARGETS] = {};
        /* The Base Pass's fifth target (RENDERPIPELINE.md, "Velocity"): each
         * pixel's world motion since the frame before, a 1 where written.
         * RGBA16F; null where the backend has no such target. */
        GDIRenderTarget* Velocity    = nullptr;
        bool             bVelocityTried = false;
        GDIRenderTarget* PostProcess = nullptr;
        GDIRenderTarget* PostProcessB = nullptr;                // the post process stack ping-pongs PostProcess and this

        /* The post process stack's smaller targets (RENDERPIPELINE.md, "Post
         * process"), made on first use: Depth of Field's two at half size, and
         * Bloom's chain, each half the size of the one before (1/2 to 1/64),
         * and its way back up, BloomUp[i] the size of Bloom[i]. */
        GDIRenderTarget* Half[2] = {};
        GDIRenderTarget* Bloom[6] = {};                         // RENDER_BLOOM_LEVELS_MAX
        GDIRenderTarget* BloomUp[5] = {};                       // levels 0 to RENDER_BLOOM_LEVELS_MAX - 2
        GDIRenderTarget* Final       = nullptr;                 // SceneColor or PostProcess: where the last RenderSceneStages finished
        /* Auto exposure (RENDERPIPELINE.md, "Auto exposure"): the meter, 64 x 64 -> 8 x 8 -> 1 x 1,
         * and the adapted value, two 1 x 1s ping-ponged so each frame eases from the last
         * (Exposure[ExposureCurrent] the newest). Fixed sizes: never resized with the scene. */
        /* Temporal antialiasing (RENDERPIPELINE.md, "Temporal antialiasing"): the resolved
         * scene colour, two full-size half floats ping-ponged -- one is this frame's
         * resolve, the other the history it blends from. Resized, and forgotten, with the scene. */
        GDIRenderTarget* History[2] = {};
        uint32_t         HistoryCurrent = 0;
        bool             bHistoryValid  = false;               // History[HistoryCurrent] holds the last frame's
        /* Screen-space reflections (RENDERPIPELINE.md): this frame's, full size, and the last
         * frame's finished linear colour they read -- TAA's history when it ran (LastColor
         * then points at it), else a copy kept in LastCopy. Forgotten with a resize. */
        GDIRenderTarget* Fog         = nullptr;                 // volumetric fog's march, half size, half float
        GDIRenderTarget* DepthCopy   = nullptr;                 // the opaque depth, for forward reflections (R32F; LIT_TRANSLUCENCY.md, Phase 4)
        GDIRenderTarget* OpaqueCopy  = nullptr;                 // the lit opaque scene before the translucent stage: forward reflections and refraction read it (Phase 10)
        GDIRenderTarget* Reflections = nullptr;
        /* Stochastic reflections' history (PPReflectionResolve.fsh; GLOBAL_ILLUMINATION.md, Phase 4):
         * the resolve ping-pongs these, ReflectionCurrent the last frame's when bReflectionValid. */
        GDIRenderTarget* ReflectionHistory[2] = {};
        uint32_t         ReflectionCurrent = 0;
        bool             bReflectionValid  = false;
        /* Screen-space GI (PPScreenGI.fsh; GLOBAL_ILLUMINATION.md, Phase 5): this frame's bounce
         * and its history, half size, half float; the resolve ping-pongs GIHistory likewise. */
        GDIRenderTarget* GI = nullptr;
        GDIRenderTarget* GIHistory[2] = {};
        uint32_t         GICurrent = 0;
        bool             bGIValid  = false;
        /* Skin (SHADING_MODELS.md): its pixels' diffuse light before albedo, as the Lighting and
         * IBL passes write it, and PPSubsurface's first blur. Full size, half float, on first use. */
        GDIRenderTarget* Subsurface     = nullptr;
        GDIRenderTarget* SubsurfaceBlur = nullptr;
        GDIRenderTarget* LastCopy    = nullptr;
        GDIRenderTarget* LastColor   = nullptr;                 // not owned: LastCopy or a History
        GDIRenderTarget* Luminance[3] = {};
        GDIRenderTarget* Exposure[2]  = {};
        uint32_t         ExposureCurrent = 0;
        bool             bExposureValid  = false;               // Exposure[ExposureCurrent] holds the last frame's
        uint             Width  = 0;
        uint             Height = 0;

        explicit SceneTargets(std::string InName) : Name(std::move(InName)) {}
        ~SceneTargets() { Release(); }

        SceneTargets(const SceneTargets&)            = delete;
        SceneTargets& operator=(const SceneTargets&) = delete;

        /* Sizes the set, creating the scene colour on first use. False for an
         * empty size or a backend that made no target. */
        bool Allocate(const uint InWidth, const uint InHeight)
        {
            if(InWidth == 0 || InHeight == 0)
            {
                return false;
            }
            if(InWidth != Width || InHeight != Height)
            {
                if(!SceneColor)
                {
                    /* Half float where the backend can: lit light past 1 survives to the tone map. */
                    bHDR = GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F);
                    SceneColor = bHDR ? GDICreateRenderTargetFormat(Name + ".SceneColor", InWidth, InHeight, GDI_RTF_RGBA16F)
                                      : GDICreateRenderTarget(Name + ".SceneColor", InWidth, InHeight);
                    bHDR = bHDR && SceneColor;
                }
                else
                {
                    SceneColor->Resize(InWidth, InHeight);
                }
                for(GDIRenderTarget* Each : GBuffer)
                {
                    if(Each) { Each->Resize(InWidth, InHeight); }
                }
                if(Velocity)
                {
                    Velocity->Resize(InWidth, InHeight);
                }
                if(PostProcess)
                {
                    PostProcess->Resize(InWidth, InHeight);
                }
                if(PostProcessB)
                {
                    PostProcessB->Resize(InWidth, InHeight);
                }
                for(GDIRenderTarget* Each : Half)
                {
                    if(Each) { Each->Resize(HalfSize(InWidth, 1), HalfSize(InHeight, 1)); }
                }
                for(GDIRenderTarget* Each : History)
                {
                    if(Each) { Each->Resize(InWidth, InHeight); }
                }
                bHistoryValid = false;
                if(Reflections) { Reflections->Resize(InWidth, InHeight); }
                for(GDIRenderTarget* Each : ReflectionHistory)
                {
                    if(Each) { Each->Resize(InWidth, InHeight); }
                }
                bReflectionValid = false;
                for(GDIRenderTarget* Each : { GI, GIHistory[0], GIHistory[1] })
                {
                    if(Each) { Each->Resize(HalfSize(InWidth, 1), HalfSize(InHeight, 1)); }
                }
                bGIValid = false;
                if(Subsurface)     { Subsurface->Resize(InWidth, InHeight); }
                if(SubsurfaceBlur) { SubsurfaceBlur->Resize(InWidth, InHeight); }
                if(Fog)         { Fog->Resize(HalfSize(InWidth, 1), HalfSize(InHeight, 1)); }
                if(DepthCopy)   { DepthCopy->Resize(HalfSize(InWidth, 1), HalfSize(InHeight, 1)); }
                if(OpaqueCopy)  { OpaqueCopy->Resize(InWidth, InHeight); }
                if(LastCopy)    { LastCopy->Resize(InWidth, InHeight); }
                LastColor = nullptr;
                for(uint32_t Level = 0; Level < 6; ++Level)
                {
                    if(Bloom[Level]) { Bloom[Level]->Resize(HalfSize(InWidth, Level + 1), HalfSize(InHeight, Level + 1)); }
                    if(Level < 5 && BloomUp[Level])
                    {
                        BloomUp[Level]->Resize(HalfSize(InWidth, Level + 1), HalfSize(InHeight, Level + 1));
                    }
                }
                Width  = InWidth;
                Height = InHeight;
            }
            return SceneColor != nullptr;
        }

        /* The five G-buffer targets, created on first use. False if any failed.
         * GBuffer1, the normal, is RGB10A2 where the backend has it (RENDERPIPELINE.md,
         * "The G-buffer"); the rest RGBA8. */
        bool EnsureGBuffer()
        {
            for(uint32_t Index = 0; Index < RENDER_GBUFFER_TARGETS; ++Index)
            {
                if(!GBuffer[Index] && Width > 0 && Height > 0)
                {
                    const std::string TargetName = Name + ".GBuffer" + std::to_string(Index);
                    GBuffer[Index] = Index == 1 && GDISupportsRenderTargetFormat(GDI_RTF_RGB10A2)
                                         ? GDICreateRenderTargetFormat(TargetName, Width, Height, GDI_RTF_RGB10A2)
                                         : GDICreateRenderTarget(TargetName, Width, Height);
                }
                if(!GBuffer[Index] || !GBuffer[Index]->IsValid())
                {
                    return false;
                }
            }
            return true;
        }

        /* The velocity target, made once with the G-buffer; null if the backend cannot. */
        GDIRenderTarget* EnsureVelocity()
        {
            if(!Velocity && !bVelocityTried && Width > 0 && Height > 0)
            {
                bVelocityTried = true;
                if(GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
                {
                    Velocity = GDICreateRenderTargetFormat(Name + ".Velocity", Width, Height, GDI_RTF_RGBA16F);
                }
            }
            return Velocity && Velocity->IsValid() ? Velocity : nullptr;
        }

        /* InSize halved InTimes times, never below 1. */
        static uint HalfSize(const uint InSize, const uint32_t InTimes)
        {
            const uint Size = InSize >> InTimes;
            return Size > 0 ? Size : 1;
        }

        GDIRenderTarget* EnsurePostProcessB()
        {
            if(!PostProcessB && Width > 0 && Height > 0)
            {
                PostProcessB = GDICreateRenderTarget(Name + ".PostProcessB", Width, Height);
            }
            return PostProcessB;
        }

        /* Depth of Field's half-size pair; false if a target cannot be made. */
        bool EnsureHalf()
        {
            for(uint32_t Index = 0; Index < 2; ++Index)
            {
                if(!Half[Index] && Width > 0 && Height > 0)
                {
                    Half[Index] = GDICreateRenderTarget(Name + ".Half" + std::to_string(Index), HalfSize(Width, 1), HalfSize(Height, 1));
                }
                if(!Half[Index] || !Half[Index]->IsValid()) { return false; }
            }
            return true;
        }

        /* Bloom's chain to InLevels (1-6), and BloomUp below the last; false if
         * a target cannot be made. */
        bool EnsureBloom(const uint32_t InLevels)
        {
            for(uint32_t Level = 0; Level < InLevels && Level < 6; ++Level)
            {
                if(!Bloom[Level] && Width > 0 && Height > 0)
                {
                    Bloom[Level] = GDICreateRenderTarget(Name + ".Bloom" + std::to_string(Level),
                                                         HalfSize(Width, Level + 1), HalfSize(Height, Level + 1));
                }
                if(!Bloom[Level] || !Bloom[Level]->IsValid()) { return false; }
                if(Level + 1 < InLevels)
                {
                    if(!BloomUp[Level] && Width > 0 && Height > 0)
                    {
                        BloomUp[Level] = GDICreateRenderTarget(Name + ".BloomUp" + std::to_string(Level),
                                                               HalfSize(Width, Level + 1), HalfSize(Height, Level + 1));
                    }
                    if(!BloomUp[Level] || !BloomUp[Level]->IsValid()) { return false; }
                }
            }
            return true;
        }

        /* Temporal antialiasing's pair, full size, half float; false if they cannot be made. */
        bool EnsureHistory()
        {
            if(!GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F) || Width == 0 || Height == 0) { return false; }
            for(uint32_t Index = 0; Index < 2; ++Index)
            {
                if(!History[Index])
                {
                    History[Index] = GDICreateRenderTargetFormat(Name + ".History" + std::to_string(Index), Width, Height, GDI_RTF_RGBA16F);
                }
                if(!History[Index] || !History[Index]->IsValid()) { return false; }
            }
            return true;
        }

        /* A full-size half-float target, made on first use: Reflections, ReflectionHistory, LastCopy, OpaqueCopy. */
        GDIRenderTarget* EnsureFullHalfFloat(GDIRenderTarget*& InOutTarget, const char* InName)
        {
            if(!InOutTarget && Width > 0 && Height > 0 && GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
            {
                InOutTarget = GDICreateRenderTargetFormat(Name + "." + InName, Width, Height, GDI_RTF_RGBA16F);
            }
            return InOutTarget && InOutTarget->IsValid() ? InOutTarget : nullptr;
        }

        /* A half-size half-float target, made on first use: GI, GIHistory. */
        GDIRenderTarget* EnsureHalfHalfFloat(GDIRenderTarget*& InOutTarget, const char* InName)
        {
            if(!InOutTarget && Width > 0 && Height > 0 && GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
            {
                InOutTarget = GDICreateRenderTargetFormat(Name + "." + InName, HalfSize(Width, 1), HalfSize(Height, 1), GDI_RTF_RGBA16F);
            }
            return InOutTarget && InOutTarget->IsValid() ? InOutTarget : nullptr;
        }

        /* Forward reflections' copy of the opaque depth, half size (the nearest of each four), R32F
         * (RGBA32F where that is all there is), made on first use; null where neither is: a half
         * float cannot hold depth. */
        GDIRenderTarget* EnsureDepthCopy()
        {
            if(!DepthCopy && Width > 0 && Height > 0)
            {
                const EGDIRenderTargetFormat Format = GDISupportsRenderTargetFormat(GDI_RTF_R32F) ? GDI_RTF_R32F
                                                    : GDISupportsRenderTargetFormat(GDI_RTF_RGBA32F) ? GDI_RTF_RGBA32F
                                                    : GDI_RTF_RGBA8;
                if(Format != GDI_RTF_RGBA8)
                {
                    DepthCopy = GDICreateRenderTargetFormat(Name + ".DepthCopy", HalfSize(Width, 1), HalfSize(Height, 1), Format);
                }
            }
            return DepthCopy && DepthCopy->IsValid() ? DepthCopy : nullptr;
        }

        /* Volumetric fog's half-size half-float target, made on first use. */
        GDIRenderTarget* EnsureFog()
        {
            if(!Fog && Width > 0 && Height > 0 && GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
            {
                Fog = GDICreateRenderTargetFormat(Name + ".Fog", HalfSize(Width, 1), HalfSize(Height, 1), GDI_RTF_RGBA16F);
            }
            return Fog && Fog->IsValid() ? Fog : nullptr;
        }

        /* Auto exposure's targets, half float; false if the backend cannot make them.
         * The adapted pair is R32F where the backend has it: a half float's step near
         * log2 8 is 1/128 of a stop, more than a frame's easing at 60 Hz moves it, so
         * the adaptation would stall short of its target. */
        bool EnsureExposure()
        {
            if(!GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F)) { return false; }
            const EGDIRenderTargetFormat Adapted = GDISupportsRenderTargetFormat(GDI_RTF_R32F) ? GDI_RTF_R32F : GDI_RTF_RGBA16F;
            const uint Sizes[3] = { 64, 8, 1 };
            for(uint32_t Index = 0; Index < 3; ++Index)
            {
                if(!Luminance[Index])
                {
                    Luminance[Index] = GDICreateRenderTargetFormat(Name + ".Luminance" + std::to_string(Sizes[Index]),
                                                                   Sizes[Index], Sizes[Index], GDI_RTF_RGBA16F);
                }
                if(!Luminance[Index] || !Luminance[Index]->IsValid()) { return false; }
            }
            for(uint32_t Index = 0; Index < 2; ++Index)
            {
                if(!Exposure[Index])
                {
                    Exposure[Index] = GDICreateRenderTargetFormat(Name + ".Exposure" + std::to_string(Index), 1, 1, Adapted);
                }
                if(!Exposure[Index] || !Exposure[Index]->IsValid()) { return false; }
            }
            return true;
        }

        GDIRenderTarget* EnsurePostProcess()
        {
            if(!PostProcess && Width > 0 && Height > 0)
            {
                PostProcess = GDICreateRenderTarget(Name + ".PostProcess", Width, Height);
            }
            return PostProcess;
        }

        void Release()
        {
            for(GDIRenderTarget** Each : { &SceneColor, &GBuffer[0], &GBuffer[1], &GBuffer[2], &GBuffer[3], &GBuffer[4], &Velocity, &PostProcess, &PostProcessB,
                                           &Half[0], &Half[1], &Bloom[0], &Bloom[1], &Bloom[2], &Bloom[3], &Bloom[4], &Bloom[5],
                                           &BloomUp[0], &BloomUp[1], &BloomUp[2], &BloomUp[3], &BloomUp[4],
                                           &Luminance[0], &Luminance[1], &Luminance[2], &Exposure[0], &Exposure[1],
                                           &History[0], &History[1], &Reflections, &ReflectionHistory[0], &ReflectionHistory[1], &GI, &GIHistory[0], &GIHistory[1], &LastCopy, &Fog, &DepthCopy, &OpaqueCopy, &Subsurface, &SubsurfaceBlur })
            {
                if(*Each)
                {
                    GDIReleaseRenderTarget(*Each);
                    delete *Each;
                    *Each = nullptr;
                }
            }
            Final  = nullptr;
            bVelocityTried = false;
            ExposureCurrent = 0;
            bExposureValid  = false;
            HistoryCurrent  = 0;
            bHistoryValid   = false;
            LastColor       = nullptr;
            Width  = 0;
            Height = 0;
        }
    };
}

#endif //RENDERCORE_SCENETARGETS_H
