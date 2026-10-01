#include "Private/RenderCore/RenderComponentProxy.h"

#include <cstring>

#include "Public/RenderCore/MeshAsset.h"
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

        /* Asked every frame, so a reimport (MeshAssetCache::Invalidate) is
         * noticed: the cache hands back a different mesh, and the bounds follow. */
        const std::string Path(Component.Mesh, strnlen(Component.Mesh, RENDER_MESH_PATH_MAX));
        std::shared_ptr<const MeshData> Source = Path.empty() ? nullptr : MeshAssetCache::Find(Path);
        const bool bSameMesh = Path == MeshPath && Source == MeshSource;
        MeshPath   = Path;
        MeshSource = std::move(Source);

        if(MeshSource)
        {
            /* The mesh's own box, in entity space; the Transform does the rest. */
            if(!bSameMesh || !bBoundsAreMesh)
            {
                SetLocalBounds(GameCore::AABB(
                    EngineCore::Vector3(MeshSource->BoundsMin[0], MeshSource->BoundsMin[1], MeshSource->BoundsMin[2]),
                    EngineCore::Vector3(MeshSource->BoundsMax[0], MeshSource->BoundsMax[1], MeshSource->BoundsMax[2])));
                bBoundsAreMesh = true;
            }
            return;
        }

        /* A unit box in entity space, half-extents (Width/2, Height/2) on X
         * and Y -- the flat quad's own local bounds, matching how
         * BoundsComponent's default unit box is used in Sandbox. Zero depth:
         * a flat quad has no Z extent to speak of. */
        SetLocalBounds(GameCore::AABB(
            EngineCore::Vector3(-double(Width) * 0.5, -double(Height) * 0.5, 0.0),
            EngineCore::Vector3(double(Width) * 0.5, double(Height) * 0.5, 0.0)));
        bBoundsAreMesh = false;
    }

    void RenderComponentProxy::Draw(RenderContext& InContext)
    {
        /* A mesh this backend cannot draw falls back to the quad. */
        if(MeshSource && InContext.DrawMesh(GetTransform(), MeshPath.c_str(), Tint, Material))
        {
            return;
        }
        if(Material[0] != '\0')
        {
            InContext.DrawQuadWithMaterial(GetTransform(), Width, Height, Tint, Material);
            return;
        }
        InContext.DrawQuad(GetTransform(), Width, Height, Tint);
    }
}
