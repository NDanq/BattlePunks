#ifndef RENDERCORE_RENDERCOMPONENTPROXY_H
#define RENDERCORE_RENDERCOMPONENTPROXY_H

#include <memory>
#include <string>

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderProxy.h"

/*
 * The render-thread mirror of a RenderComponent: its mesh, or its quad when
 * it names none (or names one that cannot be drawn).
 * Private: nothing outside this module needs to subclass THIS proxy, only
 * RenderProxy itself (a game's own proxy type registers through
 * RegisterProxyType the same way RenderComponent does, in Renderer::Initialize).
 */
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

    private:
        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        bool              bComponentVisible = true;
        char              Material[48] = {};   // RENDER_MATERIAL_NAME_MAX; "" = the plain tint

        /* The component's Mesh, and the mesh it found -- kept so the bounds
         * are only redone when either changes. Null source: draw the quad. */
        std::string                            MeshPath;
        std::shared_ptr<const MeshData>        MeshSource;
        bool                                   bBoundsAreMesh = false;
    };
}

#endif //RENDERCORE_RENDERCOMPONENTPROXY_H
