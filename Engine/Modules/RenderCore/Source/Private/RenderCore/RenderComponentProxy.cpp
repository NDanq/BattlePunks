#include "Private/RenderCore/RenderComponentProxy.h"

#include <algorithm>
#include <cmath>
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
        bCastShadows      = Component.bCastShadows != 0;
        bCastInsetShadow  = Component.bCastInsetShadow != 0;
        bOccluder         = Component.bOccluder != 0;

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

        bSkinned = MeshSource && MeshSource->IsSkinned();
        if(MeshSource)
        {
            /* The mesh's own box, in entity space; the Transform does the rest.
             * A skinned mesh's follows its pose (RebuildSkin). */
            if(bSkinned && bSameMesh)
            {
                return;   // posed bounds, kept by RebuildSkin
            }
            if(!bSameMesh || !bBoundsAreMesh)
            {
                SetLocalBounds(GameCore::AABB(
                    EngineCore::Vector3(MeshSource->BoundsMin[0], MeshSource->BoundsMin[1], MeshSource->BoundsMin[2]),
                    EngineCore::Vector3(MeshSource->BoundsMax[0], MeshSource->BoundsMax[1], MeshSource->BoundsMax[2])));
                bBoundsAreMesh = true;
                if(bSkinned) { RebuildSkin(); }
            }
            return;
        }
        SkinPose.clear();
        SkinPalette.clear();

        /* The unit quad's local bounds. */
        SetLocalBounds(GameCore::AABB(EngineCore::Vector3(-0.5, -0.5, 0.0), EngineCore::Vector3(0.5, 0.5, 0.0)));
        bBoundsAreMesh = false;
    }

    void RenderComponentProxy::SetSkinPose(const AnimMatrix* InBones, const uint32_t InCount)
    {
        SkinPose.assign(InBones, InBones + InCount);
        if(bSkinned)
        {
            RebuildSkin();
        }
    }

    void RenderComponentProxy::EndFrameMotion()
    {
        RenderProxy::EndFrameMotion();
        PrevSkinPalette = SkinPalette;
    }

    void RenderComponentProxy::RebuildSkin()
    {
        const MeshData& Mesh = *MeshSource;
        const size_t    Bones = std::min(SkinPose.size(), Mesh.InverseBind.size());
        if(Bones == 0)
        {
            /* No pose: the bind pose, in the mesh's own box. */
            SkinPalette.clear();
            SetLocalBounds(GameCore::AABB(EngineCore::Vector3(Mesh.BoundsMin[0], Mesh.BoundsMin[1], Mesh.BoundsMin[2]),
                                          EngineCore::Vector3(Mesh.BoundsMax[0], Mesh.BoundsMax[1], Mesh.BoundsMax[2])));
            bBoundsAreMesh = true;
            return;
        }

        /* Each bone's reach, measured once per mesh in its bind pose: the
         * farthest vertex it moves, from where the bone binds. */
        if(RadiiFor != &Mesh)
        {
            RadiiFor = &Mesh;
            BoneRadii.assign(Mesh.InverseBind.size(), -1.0f);
            for(uint32_t Vertex = 0; Vertex < Mesh.GetVertexCount(); ++Vertex)
            {
                const AnimVec3 P{ Mesh.Positions[size_t(Vertex) * 3], Mesh.Positions[size_t(Vertex) * 3 + 1], Mesh.Positions[size_t(Vertex) * 3 + 2] };
                for(int Each = 0; Each < 4; ++Each)
                {
                    const size_t Influence = size_t(Vertex) * 4 + size_t(Each);
                    if(Mesh.Weights[Influence] <= 0.0f) { continue; }
                    const uint16_t Bone = Mesh.Joints[Influence];
                    const AnimVec3 Local = AnimMath::TransformPoint(Mesh.InverseBind[Bone], P);
                    const float    Reach = std::sqrt(Local.X * Local.X + Local.Y * Local.Y + Local.Z * Local.Z);
                    BoneRadii[Bone] = std::max(BoneRadii[Bone], Reach);
                }
            }
        }

        SkinPalette.resize(Bones * 12);
        bool  bAny = false;
        float Min[3] = {0.0f, 0.0f, 0.0f}, Max[3] = {0.0f, 0.0f, 0.0f};
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            const AnimMatrix Skin = AnimMath::Mul(SkinPose[Bone], Mesh.InverseBind[Bone]);
            std::memcpy(&SkinPalette[Bone * 12], Skin.M, sizeof(Skin.M));

            /* A sphere of the bone's reach round where it is now. Scale is
             * taken as its largest axis. */
            if(BoneRadii[Bone] < 0.0f) { continue; }
            const AnimMatrix& Pose = SkinPose[Bone];
            float Scale = 0.0f;
            for(int Col = 0; Col < 3; ++Col)
            {
                Scale = std::max(Scale, std::sqrt(Pose.M[0][Col] * Pose.M[0][Col] + Pose.M[1][Col] * Pose.M[1][Col] + Pose.M[2][Col] * Pose.M[2][Col]));
            }
            const float Reach = BoneRadii[Bone] * Scale;
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const float At = Pose.M[Axis][3];
                Min[Axis] = bAny ? std::min(Min[Axis], At - Reach) : At - Reach;
                Max[Axis] = bAny ? std::max(Max[Axis], At + Reach) : At + Reach;
            }
            bAny = true;
        }
        if(bAny)
        {
            SetLocalBounds(GameCore::AABB(EngineCore::Vector3(Min[0], Min[1], Min[2]), EngineCore::Vector3(Max[0], Max[1], Max[2])));
            bBoundsAreMesh = false;   // the bind box again, should the pose go
        }
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
        Overrides.Baked = &GetBakedDraw();
        if(bSkinned && !SkinPalette.empty())
        {
            Overrides.SkinPalette = SkinPalette.data();
            Overrides.SkinBones   = uint32_t(SkinPalette.size() / 12);
            Overrides.PrevSkinPalette = PrevSkinPalette.size() == SkinPalette.size() ? PrevSkinPalette.data() : nullptr;
        }

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

    void RenderComponentProxy::DrawShadow(RenderContext& InContext)
    {
        /* A mesh only: a quad is a 2D sprite, and casts nothing. */
        if(bCastShadows && MeshSource)
        {
            DrawDepth(InContext, /*bInOccluder*/ false);
        }
    }

    void RenderComponentProxy::DrawOccluder(RenderContext& InContext)
    {
        if(IsOccluder())
        {
            DrawDepth(InContext, /*bInOccluder*/ true);
        }
    }

    void RenderComponentProxy::DrawDepth(RenderContext& InContext, const bool bInOccluder)
    {
        MaterialParameterOverrides Overrides;
        Overrides.Items = Parameters;
        Overrides.Count = ParameterCount;
        if(bSkinned && !SkinPalette.empty())
        {
            Overrides.SkinPalette = SkinPalette.data();
            Overrides.SkinBones   = uint32_t(SkinPalette.size() / 12);
            Overrides.PrevSkinPalette = PrevSkinPalette.size() == SkinPalette.size() ? PrevSkinPalette.data() : nullptr;
        }

        const char* Slots[RENDER_MATERIAL_SLOTS];
        for(size_t Slot = 0; Slot < RENDER_MATERIAL_SLOTS; ++Slot) { Slots[Slot] = Materials[Slot]; }
        if(bInOccluder)
        {
            InContext.DrawMeshOccluder(GetTransform(), MeshPath.c_str(), Slots, uint32_t(RENDER_MATERIAL_SLOTS), Overrides);
        }
        else
        {
            InContext.DrawMeshShadow(GetTransform(), MeshPath.c_str(), Slots, uint32_t(RENDER_MATERIAL_SLOTS), Overrides);
        }
    }
}
