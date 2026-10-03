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

    struct SceneTargets
    {
        std::string      Name;                                  // a prefix for the targets' debug names
        GDIRenderTarget* SceneColor  = nullptr;
        GDIRenderTarget* GBuffer[RENDER_GBUFFER_TARGETS] = {};
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
                    SceneColor = GDICreateRenderTarget(Name + ".SceneColor", InWidth, InHeight);
                }
                else
                {
                    SceneColor->Resize(InWidth, InHeight);
                }
                for(GDIRenderTarget* Each : GBuffer)
                {
                    if(Each) { Each->Resize(InWidth, InHeight); }
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
            for(GDIRenderTarget** Each : { &SceneColor, &GBuffer[0], &GBuffer[1], &GBuffer[2], &GBuffer[3], &PostProcess, &PostProcessB,
                                           &Half[0], &Half[1], &Bloom[0], &Bloom[1], &Bloom[2], &Bloom[3], &Bloom[4], &Bloom[5],
                                           &BloomUp[0], &BloomUp[1], &BloomUp[2], &BloomUp[3], &BloomUp[4] })
            {
                if(*Each)
                {
                    GDIReleaseRenderTarget(*Each);
                    delete *Each;
                    *Each = nullptr;
                }
            }
            Final  = nullptr;
            Width  = 0;
            Height = 0;
        }
    };
}

#endif //RENDERCORE_SCENETARGETS_H
