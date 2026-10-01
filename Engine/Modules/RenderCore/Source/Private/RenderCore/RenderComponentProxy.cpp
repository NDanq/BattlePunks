#include "Private/RenderCore/RenderComponentProxy.h"

#include <algorithm>
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

        /* Bounded copy: the component's buffers need not be NUL-terminated. */
        static_assert(sizeof(Materials) == sizeof(Component.Materials), "RenderComponentProxy::Materials must match the component's");
        std::memcpy(Materials, Component.Materials, sizeof(Materials));
        for(char (&Slot)[RENDER_MATERIAL_NAME_MAX] : Materials)
        {
            Slot[RENDER_MATERIAL_NAME_MAX - 1] = '\0';
        }

        SetVisible(bComponentVisible);

        /* Every frame, so a reimport's new mesh is noticed. */
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

        /* The flat quad's local bounds: (Width/2, Height/2, 0) half-extents. */
        SetLocalBounds(GameCore::AABB(
            EngineCore::Vector3(-double(Width) * 0.5, -double(Height) * 0.5, 0.0),
            EngineCore::Vector3(double(Width) * 0.5, double(Height) * 0.5, 0.0)));
        bBoundsAreMesh = false;
    }

    float RenderComponentProxy::GetBoundsPadding(RenderContext& InContext)
    {
        /* The furthest any material it draws with reaches: each section's
         * slot when set (one that fails to load pads by nothing, like the
         * fallback it draws), else the section's own. A quad: slot 0. */
        if(!MeshSource)
        {
            return InContext.GetMaterialMaxOffset(Materials[0]);
        }
        float Padding = 0.0f;
        const size_t SectionCount = std::max<size_t>(MeshSource->Sections.size(), 1);
        for(size_t Index = 0; Index < SectionCount; ++Index)
        {
            if(Index < RENDER_MATERIAL_SLOTS && Materials[Index][0] != '\0')
            {
                Padding = std::max(Padding, InContext.GetMaterialMaxOffset(Materials[Index]));
            }
            else if(Index < MeshSource->Sections.size())
            {
                Padding = std::max(Padding, InContext.GetMaterialMaxOffset(MeshSource->Sections[Index].Material.c_str(),
                                                                           /*bQuietIfMissing*/ true));
            }
        }
        return Padding;
    }

    void RenderComponentProxy::Draw(RenderContext& InContext)
    {
        const char* Slots[RENDER_MATERIAL_SLOTS];
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot) { Slots[Slot] = Materials[Slot]; }

        /* A mesh this backend cannot draw falls back to the quad. */
        if(MeshSource && InContext.DrawMesh(GetTransform(), MeshPath.c_str(), Tint, Slots, uint32_t(RENDER_MATERIAL_SLOTS)))
        {
            return;
        }
        if(Materials[0][0] != '\0')
        {
            InContext.DrawQuadWithMaterial(GetTransform(), Width, Height, Tint, Materials[0]);
            return;
        }
        InContext.DrawQuad(GetTransform(), Width, Height, Tint);
    }
}
