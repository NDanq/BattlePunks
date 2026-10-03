#include "Private/RenderCore/RenderComponentProxy.h"

#include <algorithm>
#include <cstring>

#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/RenderComponents.h"

namespace RenderCore
{
    namespace
    {
        /* The quad is 1 x 1 (the Transform sizes it); Instance Tint reads white. */
        constexpr float             QUAD_SIZE = 1.0f;
        const EngineCore::Color     WHITE{255, 255, 255};
    }

    RenderComponentProxy::RenderComponentProxy(const GameCore::EntityID InOwner, const RenderComponent& InInit)
        : RenderProxy(InOwner)
    {
        ApplyComponent(&InInit);
    }

    void RenderComponentProxy::ApplyComponent(const void* InComponentBytes)
    {
        const auto& Component = *static_cast<const RenderComponent*>(InComponentBytes);

        bComponentVisible = Component.bVisible != 0;

        /* Bounded copy: the component's buffers need not be NUL-terminated. */
        static_assert(sizeof(Materials) == sizeof(Component.Materials), "RenderComponentProxy::Materials must match the component's");
        std::memcpy(Materials, Component.Materials, sizeof(Materials));
        for(char (&Slot)[RENDER_MATERIAL_PATH_MAX] : Materials)
        {
            Slot[RENDER_MATERIAL_PATH_MAX - 1] = '\0';
        }

        static_assert(sizeof(Parameters) == sizeof(Component.Parameters), "RenderComponentProxy::Parameters must match the component's");
        std::memcpy(Parameters, Component.Parameters, sizeof(Parameters));
        /* Up to the last in use; BindMaterial skips empty ones between. */
        ParameterCount = 0;
        for(size_t Index = 0; Index < RENDER_MATERIAL_PARAMETERS_MAX; ++Index)
        {
            Parameters[Index].Name[RENDER_MATERIAL_PARAMETER_NAME_MAX - 1] = '\0';
            if(Parameters[Index].Name[0] != '\0') { ParameterCount = uint32_t(Index + 1); }
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

        /* The unit quad's local bounds. */
        SetLocalBounds(GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0)));
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
        MaterialParameterOverrides Overrides;
        Overrides.Items = Parameters;
        Overrides.Count = ParameterCount;

        const char* Slots[RENDER_MATERIAL_SLOTS];
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot) { Slots[Slot] = Materials[Slot]; }

        /* A mesh this backend cannot draw falls back to the quad. */
        if(MeshSource && InContext.DrawMesh(GetTransform(), MeshPath.c_str(), WHITE, Slots, uint32_t(RENDER_MATERIAL_SLOTS), Overrides))
        {
            return;
        }
        if(Materials[0][0] != '\0')
        {
            InContext.DrawQuadWithMaterial(GetTransform(), QUAD_SIZE, QUAD_SIZE, WHITE, Materials[0], Overrides);
            return;
        }
        InContext.DrawQuad(GetTransform(), QUAD_SIZE, QUAD_SIZE, WHITE);
    }
}
