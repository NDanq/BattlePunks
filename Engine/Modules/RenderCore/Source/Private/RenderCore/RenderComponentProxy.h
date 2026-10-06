#ifndef RENDERCORE_RENDERCOMPONENTPROXY_H
#define RENDERCORE_RENDERCOMPONENTPROXY_H

#include <memory>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/Components/RenderComponent.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderProxy.h"

/* The render-thread mirror of a RenderComponent: its mesh, or else a unit quad. */
namespace RenderCore
{
    struct MeshData;

    class RenderComponentProxy final : public RenderProxy
    {
    public:
        RenderComponentProxy(GameCore::EntityID InOwner, const RenderComponent& InInit);

        void ApplyComponent(const void* InComponentBytes) override;
        void Draw(RenderContext& InContext) override;
        void DrawShadow(RenderContext& InContext) override;
        float GetBoundsPadding(RenderContext& InContext) override;
        bool CastsInsetShadow() const override { return bCastShadows && bCastInsetShadow && MeshSource; }

    private:
        bool              bComponentVisible = true;
        bool              bCastShadows      = true;
        bool              bCastInsetShadow  = false;
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
