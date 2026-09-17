#include "Private/RenderCore/PrimitiveProxy.h"

#include "Public/RenderCore/RenderComponents.h"

namespace RenderCore
{
    PrimitiveProxy::PrimitiveProxy(const GameCore::EntityID InOwner, const PrimitiveComponent& InInit)
        : RenderProxy(InOwner)
    {
        ApplyComponent(&InInit);
    }

    void PrimitiveProxy::ApplyComponent(const void* InComponentBytes)
    {
        const auto& Component = *static_cast<const PrimitiveComponent*>(InComponentBytes);

        Width             = Component.Width;
        Height            = Component.Height;
        Tint              = Component.Tint;
        bComponentVisible = Component.bVisible != 0;

        SetVisible(bComponentVisible);

        /* A unit box in entity space, half-extents (Width/2, Height/2) on X
         * and Y -- the flat quad's own local bounds, matching how
         * BoundsComponent's default unit box is used in Sandbox. Zero depth:
         * a flat quad has no Z extent to speak of. */
        SetLocalBounds(GameCore::AABB(
            EngineCore::Vector3(-double(Width) * 0.5, -double(Height) * 0.5, 0.0),
            EngineCore::Vector3(double(Width) * 0.5, double(Height) * 0.5, 0.0)));
    }

    void PrimitiveProxy::Draw(RenderContext& InContext)
    {
        InContext.DrawQuad(GetTransform(), Width, Height, Tint);
    }
}
