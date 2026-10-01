#ifndef RENDERCORE_RENDERCOMPONENTPROXY_H
#define RENDERCORE_RENDERCOMPONENTPROXY_H

#include <memory>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderProxy.h"

/* The render-thread mirror of a RenderComponent: its mesh, or else its quad. */
namespace RenderCore
{
    struct RenderComponent;
    struct MeshData;

    class RenderComponentProxy final : public RenderProxy
    {
    public:
        RenderComponentProxy(GameCore::EntityID InOwner, const RenderComponent& InInit);

        void ApplyComponent(const void* InComponentBytes) override;
        void Draw(RenderContext& InContext) override;
        float GetBoundsPadding(RenderContext& InContext) override;

    private:
        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        bool              bComponentVisible = true;
        char              Material[48] = {};   // RENDER_MATERIAL_NAME_MAX; "" = the plain tint

        /* Bounds are redone only when these change. Null source: the quad. */
        std::string                            MeshPath;
        std::shared_ptr<const MeshData>        MeshSource;
        bool                                   bBoundsAreMesh = false;
    };
}

#endif //RENDERCORE_RENDERCOMPONENTPROXY_H
