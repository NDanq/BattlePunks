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
            for(GDIRenderTarget** Each : { &SceneColor, &GBuffer[0], &GBuffer[1], &GBuffer[2], &GBuffer[3], &PostProcess })
            {
                if(*Each)
                {
                    GDIReleaseRenderTarget(*Each);
                    delete *Each;
                    *Each = nullptr;
                }
            }
            Width  = 0;
            Height = 0;
        }
    };
}

#endif //RENDERCORE_SCENETARGETS_H
