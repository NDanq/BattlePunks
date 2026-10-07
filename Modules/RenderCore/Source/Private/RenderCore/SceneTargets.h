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
    enum : uint32_t { RENDER_GBUFFER_TARGETS = 4 };

    /* A scene's image-based lighting targets (RENDERPIPELINE.md, "Image-based
     * lighting"), made by RenderContext::EnsureIBLResources: the probe atlas
     * (RGBA16F, IBLCommon.glsl's layout) and the cube strip a capture or the
     * sky is prefiltered from. The scene owns them; ReleaseIBLResources frees them. */
    struct IBLResources
    {
        GDIRenderTarget* Atlas     = nullptr;
        GDIRenderTarget* Strip     = nullptr;
        uint32_t         StripFace = 0;
    };

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
        GDIRenderTarget* Reflections = nullptr;
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
                if(Fog)         { Fog->Resize(HalfSize(InWidth, 1), HalfSize(InHeight, 1)); }
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

        /* The four G-buffer targets, created on first use. False if any failed. */
        bool EnsureGBuffer()
        {
            for(uint32_t Index = 0; Index < RENDER_GBUFFER_TARGETS; ++Index)
            {
                if(!GBuffer[Index] && Width > 0 && Height > 0)
                {
                    GBuffer[Index] = GDICreateRenderTarget(Name + ".GBuffer" + std::to_string(Index), Width, Height);
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

        /* A full-size half-float target, made on first use: Reflections, LastCopy. */
        GDIRenderTarget* EnsureFullHalfFloat(GDIRenderTarget*& InOutTarget, const char* InName)
        {
            if(!InOutTarget && Width > 0 && Height > 0 && GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F))
            {
                InOutTarget = GDICreateRenderTargetFormat(Name + "." + InName, Width, Height, GDI_RTF_RGBA16F);
            }
            return InOutTarget && InOutTarget->IsValid() ? InOutTarget : nullptr;
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

        /* Auto exposure's targets, half float; false if the backend cannot make them. */
        bool EnsureExposure()
        {
            if(!GDISupportsRenderTargetFormat(GDI_RTF_RGBA16F)) { return false; }
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
                    Exposure[Index] = GDICreateRenderTargetFormat(Name + ".Exposure" + std::to_string(Index), 1, 1, GDI_RTF_RGBA16F);
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
            for(GDIRenderTarget** Each : { &SceneColor, &GBuffer[0], &GBuffer[1], &GBuffer[2], &GBuffer[3], &Velocity, &PostProcess, &PostProcessB,
                                           &Half[0], &Half[1], &Bloom[0], &Bloom[1], &Bloom[2], &Bloom[3], &Bloom[4], &Bloom[5],
                                           &BloomUp[0], &BloomUp[1], &BloomUp[2], &BloomUp[3], &BloomUp[4],
                                           &Luminance[0], &Luminance[1], &Luminance[2], &Exposure[0], &Exposure[1],
                                           &History[0], &History[1], &Reflections, &LastCopy, &Fog })
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
