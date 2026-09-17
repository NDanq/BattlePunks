#ifndef RENDERCORE_PRIMITIVEPROXY_H
#define RENDERCORE_PRIMITIVEPROXY_H

#include "Public/Core/CoreTypes.h"
#include "Public/GameCore/AABB.h"
#include "Public/GameCore/EntityID.h"
#include "Public/RenderCore/RenderContext.h"
#include "Public/RenderCore/RenderProxy.h"

/*
 * The quad proxy -- the render-thread mirror of a PrimitiveComponent.
 * Private: nothing outside this module needs to subclass THIS proxy, only
 * RenderProxy itself (a game's own proxy type registers through
 * RegisterProxyType the same way PrimitiveComponent does, in Renderer::Initialize).
 */
namespace RenderCore
{
    struct PrimitiveComponent;

    class PrimitiveProxy final : public RenderProxy
    {
    public:
        PrimitiveProxy(GameCore::EntityID InOwner, const PrimitiveComponent& InInit);

        void ApplyComponent(const void* InComponentBytes) override;
        void Draw(RenderContext& InContext) override;

    private:
        float             Width  = 1.0f;
        float             Height = 1.0f;
        EngineCore::Color Tint{255, 255, 255};
        bool              bComponentVisible = true;
    };
}

#endif //RENDERCORE_PRIMITIVEPROXY_H
