#include "Private/RenderCore/RenderComponentProxy.h"

#include <cstring>

#include "Public/RenderCore/RenderComponents.h"

namespace RenderCore
{
    RenderComponentProxy::RenderComponentProxy(const GameCore::EntityID InOwner, const RenderComponent& InInit)
        : RenderProxy(InOwner)
    {
        ApplyComponent(&InInit);
    }

    void RenderComponentProxy::ApplyComponent(const void* InComponentBytes)
    {
        const auto& Component = *static_cast<const RenderComponent*>(InComponentBytes);

        Width             = Component.Width;
        Height            = Component.Height;
        Tint              = Component.Tint;
        bComponentVisible = Component.bVisible != 0;

        /* Bounded copy: the component's buffer need not be NUL-terminated. */
        static_assert(sizeof(Material) == RENDER_MATERIAL_NAME_MAX, "RenderComponentProxy::Material must match the component's");
        std::memcpy(Material, Component.Material, sizeof(Material));
        Material[sizeof(Material) - 1] = '\0';

        SetVisible(bComponentVisible);

        /* A unit box in entity space, half-extents (Width/2, Height/2) on X
         * and Y -- the flat quad's own local bounds, matching how
         * BoundsComponent's default unit box is used in Sandbox. Zero depth:
         * a flat quad has no Z extent to speak of. */
        SetLocalBounds(GameCore::AABB(
            EngineCore::Vector3(-double(Width) * 0.5, -double(Height) * 0.5, 0.0),
            EngineCore::Vector3(double(Width) * 0.5, double(Height) * 0.5, 0.0)));
    }

    void RenderComponentProxy::Draw(RenderContext& InContext)
    {
        if(Material[0] != '\0')
        {
            InContext.DrawQuadWithMaterial(GetTransform(), Width, Height, Tint, Material);
            return;
        }
        InContext.DrawQuad(GetTransform(), Width, Height, Tint);
    }
}
