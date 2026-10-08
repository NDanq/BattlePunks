#ifndef RENDERCORE_RENDERCOMPONENTPROXY_H
#define RENDERCORE_RENDERCOMPONENTPROXY_H

#include <memory>
#include <string>
#include <vector>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/AnimMath.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderCoreAPI.h"
#include "Public/RenderCore/RenderProxy.h"

/* The render-thread mirror of a RenderComponent: its mesh, or else a unit quad. */
namespace RenderCore
{
    struct MeshData;

    class RenderComponentProxy final : public RenderProxy
    {
    public:
        // RENDERCORE_API: RenderCoreAnimationTests builds a proxy (TestPosedBounds)
        RENDERCORE_API RenderComponentProxy(GameCore::EntityID InOwner, const RenderComponent& InInit);

        void ApplyComponent(const void* InComponentBytes) override;
        void Draw(RenderContext& InContext) override;
        void DrawShadow(RenderContext& InContext) override;
        float GetBoundsPadding(RenderContext& InContext) override;
        bool CastsInsetShadow() const override { return bCastShadows && bCastInsetShadow && MeshSource; }

        /* A mesh only: a quad is a sprite, too thin to hide anything. */
        bool IsOccluder() const override { return bOccluder && MeshSource; }
        void DrawOccluder(RenderContext& InContext) override;

        /* A skinned mesh moves every frame on its own: never a static draw. */
        bool AllowsStaticDraws() const override { return !bSkinned; }

        /* RENDER THREAD, from WorldBridge after ApplyComponent: its skeleton's
         * pose this frame, model space, one matrix per bone (ANIMATION.md).
         * InCount 0: the mesh's bind pose. */
        RENDERCORE_API void SetSkinPose(const AnimMatrix* InBones, uint32_t InCount);

        /* And its palette: this frame's is the next one's previous. */
        void EndFrameMotion() override;

    private:
        /* Its mesh's depth-writing sections, as shadow casters or as an occluder. */
        void DrawDepth(RenderContext& InContext, bool bInOccluder);

        /* The palette from SkinPose and the mesh's inverse binds, and the
         * local bounds that hold the posed mesh. */
        void RebuildSkin();

        bool                     bSkinned = false;
        std::vector<AnimMatrix>  SkinPose;           // model space, by skeleton bone; empty: the bind pose
        std::vector<float>       SkinPalette;        // 12 floats a bone: pose times inverse bind
        std::vector<float>       PrevSkinPalette;    // as SkinPalette was the frame before (EndFrameMotion); velocity's
        std::vector<float>       BoneRadii;          // how far each bone's vertices reach from it; < 0: none
        const MeshData*          RadiiFor = nullptr; // the mesh BoneRadii were measured on

        bool              bComponentVisible = true;
        bool              bCastShadows      = true;
        bool              bCastInsetShadow  = false;
        bool              bOccluder         = false;
        char              Materials[8][128] = {};  // RENDER_MATERIAL_SLOTS x RENDER_MATERIAL_PATH_MAX; "" = none

        /* The component's parameter overrides; ParameterCount: up to the last in use. */
        RenderMaterialParameter Parameters[8] = {};   // RENDER_MATERIAL_PARAMETERS_MAX
        uint32_t                ParameterCount = 0;

        /* Bounds are redone only when these change. Null source: the quad. */
        std::string                            MeshPath;
        std::shared_ptr<const MeshData>        MeshSource;
        bool                                   bBoundsAreMesh = false;
    };
}

#endif //RENDERCORE_RENDERCOMPONENTPROXY_H
