#include "Public/RenderCore/MeshAsset.h"

#include <atomic>
#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/RenderCoreLog.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Private/RenderCore/AssetFileCommon.h"

namespace RenderCore
{
    namespace
    {
        constexpr uint32_t TAG_MESH     = EngineCore::MakeArchiveTag('N','M','S','H');
        constexpr uint32_t TAG_HEADER   = EngineCore::MakeArchiveTag('M','H','D','R');
        constexpr uint32_t TAG_VERTICES = EngineCore::MakeArchiveTag('M','V','T','X');
        constexpr uint32_t TAG_INDICES  = EngineCore::MakeArchiveTag('M','I','D','X');
        constexpr uint32_t TAG_SECTIONS = EngineCore::MakeArchiveTag('M','S','E','C');
        constexpr uint32_t TAG_COLORS   = EngineCore::MakeArchiveTag('M','C','O','L');   // optional
        constexpr uint32_t TAG_UV1      = EngineCore::MakeArchiveTag('M','U','V','1');   // optional
        constexpr uint32_t TAG_SKIN     = EngineCore::MakeArchiveTag('M','S','K','N');   // optional

        void SerializeFloats(EngineCore::Archive& Ar, std::vector<float>& InOutValues)
        {
            for(float& Value : InOutValues) { Ar << Value; }
        }

        /* MHDR's fields, shared by the full load and LoadMeshHeader. */
        void SerializeHeader(EngineCore::Archive& Ar, MeshHeader& InOutHeader)
        {
            Ar << InOutHeader.VertexCount << InOutHeader.IndexCount << InOutHeader.SectionCount;
            for(int Axis = 0; Axis < 3; ++Axis) { Ar << InOutHeader.BoundsMin[Axis]; }
            for(int Axis = 0; Axis < 3; ++Axis) { Ar << InOutHeader.BoundsMax[Axis]; }
        }

        /* Why a draw of InMesh could read out of bounds; null if it cannot. */
        const char* FindDefect(const MeshData& InMesh)
        {
            const uint32_t VertexCount = InMesh.GetVertexCount();
            if(InMesh.Indices.size() % 3 != 0) { return "mesh: index count is not a multiple of 3"; }
            for(const uint32_t Index : InMesh.Indices)
            {
                if(Index >= VertexCount) { return "mesh: an index is past the last vertex"; }
            }
            for(const MeshSection& Section : InMesh.Sections)
            {
                if(uint64_t(Section.FirstIndex) + Section.IndexCount > InMesh.Indices.size())
                {
                    return "mesh: a section runs past the last index";
                }
            }
            if(InMesh.IsSkinned())
            {
                if(InMesh.Joints.size() != size_t(VertexCount) * 4 || InMesh.Weights.size() != size_t(VertexCount) * 4)
                {
                    return "mesh: skinning needs four bones and weights per vertex";
                }
                for(const uint16_t Joint : InMesh.Joints)
                {
                    if(Joint >= InMesh.InverseBind.size()) { return "mesh: a vertex names a bone past the last"; }
                }
            }
            return nullptr;
        }
    }

    void ComputeMeshBounds(MeshData& InOutMesh)
    {
        if(InOutMesh.Positions.size() < 3)
        {
            std::fill(std::begin(InOutMesh.BoundsMin), std::end(InOutMesh.BoundsMin), 0.0f);
            std::fill(std::begin(InOutMesh.BoundsMax), std::end(InOutMesh.BoundsMax), 0.0f);
            return;
        }
        for(int Axis = 0; Axis < 3; ++Axis)
        {
            InOutMesh.BoundsMin[Axis] = InOutMesh.Positions[size_t(Axis)];
            InOutMesh.BoundsMax[Axis] = InOutMesh.Positions[size_t(Axis)];
        }
        for(size_t At = 0; At + 2 < InOutMesh.Positions.size(); At += 3)
        {
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                const float Value = InOutMesh.Positions[At + size_t(Axis)];
                InOutMesh.BoundsMin[Axis] = std::min(InOutMesh.BoundsMin[Axis], Value);
                InOutMesh.BoundsMax[Axis] = std::max(InOutMesh.BoundsMax[Axis], Value);
            }
        }
    }

    void MakeRefPosePalette(const MeshData& InMesh, const SkeletonData& InSkeleton, std::vector<float>& OutPalette)
    {
        OutPalette.clear();
        if(!InMesh.IsSkinned())
        {
            return;
        }
        std::vector<AnimMatrix> Model;
        InSkeleton.ComputeRefPoseModel(Model);
        const size_t Bones = InMesh.InverseBind.size();
        OutPalette.assign(Bones * 12, 0.0f);
        for(size_t Bone = 0; Bone < Bones; ++Bone)
        {
            /* A bone the skeleton lacks (a mismatched pair) stays where it binds. */
            const AnimMatrix Skin = Bone < Model.size() ? AnimMath::Mul(Model[Bone], InMesh.InverseBind[Bone]) : AnimMatrix();
            std::memcpy(&OutPalette[Bone * 12], Skin.M, sizeof(Skin.M));
        }
    }

    void ComputeSkinnedMeshBounds(MeshData& InOutMesh, const SkeletonData& InSkeleton)
    {
        std::vector<float> Palette;
        MakeRefPosePalette(InOutMesh, InSkeleton, Palette);
        if(Palette.empty() || InOutMesh.GetVertexCount() == 0)
        {
            return;
        }
        for(uint32_t Vertex = 0; Vertex < InOutMesh.GetVertexCount(); ++Vertex)
        {
            const AnimVec3 P{ InOutMesh.Positions[size_t(Vertex) * 3], InOutMesh.Positions[size_t(Vertex) * 3 + 1],
                              InOutMesh.Positions[size_t(Vertex) * 3 + 2] };
            AnimVec3 Posed;
            for(int Each = 0; Each < 4; ++Each)
            {
                const float Weight = InOutMesh.Weights[size_t(Vertex) * 4 + size_t(Each)];
                if(Weight <= 0.0f) { continue; }
                AnimMatrix Skin;
                std::memcpy(Skin.M, &Palette[size_t(InOutMesh.Joints[size_t(Vertex) * 4 + size_t(Each)]) * 12], sizeof(Skin.M));
                Posed = AnimMath::Add(Posed, AnimMath::Scale(AnimMath::TransformPoint(Skin, P), Weight));
            }
            const float At[3] = { Posed.X, Posed.Y, Posed.Z };
            for(int Axis = 0; Axis < 3; ++Axis)
            {
                InOutMesh.BoundsMin[Axis] = Vertex == 0 ? At[Axis] : std::min(InOutMesh.BoundsMin[Axis], At[Axis]);
                InOutMesh.BoundsMax[Axis] = Vertex == 0 ? At[Axis] : std::max(InOutMesh.BoundsMax[Axis], At[Axis]);
            }
        }
    }

    bool SerializeMesh(EngineCore::Archive& Ar, MeshData& InOutMesh)
    {
        if(!OpenRequiredChunk(Ar, TAG_MESH, 1, "not a mesh (no NMSH chunk)")) { return false; }

        /* Counts first, to size the arrays. */
        MeshHeader Header;
        if(Ar.IsSaving())
        {
            Header.VertexCount  = InOutMesh.GetVertexCount();
            Header.IndexCount   = uint32_t(InOutMesh.Indices.size());
            Header.SectionCount = uint32_t(InOutMesh.Sections.size());
            std::copy(std::begin(InOutMesh.BoundsMin), std::end(InOutMesh.BoundsMin), Header.BoundsMin);
            std::copy(std::begin(InOutMesh.BoundsMax), std::end(InOutMesh.BoundsMax), Header.BoundsMax);

            /* Normals and UVs are per vertex, always -- the importer's job.
             * Colours are all or none. */
            if(InOutMesh.Normals.size() != size_t(Header.VertexCount) * 3
               || InOutMesh.UV0.size() != size_t(Header.VertexCount) * 2
               || (!InOutMesh.Colors.empty() && InOutMesh.Colors.size() != size_t(Header.VertexCount) * 4)
               || (!InOutMesh.UV1.empty() && InOutMesh.UV1.size() != size_t(Header.VertexCount) * 2)
               || (InOutMesh.IsSkinned() && (InOutMesh.Joints.size() != size_t(Header.VertexCount) * 4
                                             || InOutMesh.Weights.size() != size_t(Header.VertexCount) * 4)))
            {
                Ar.SetError("mesh: every vertex needs a normal and a UV (and a colour, a UV1 and bones, if any has them)");
                Ar.CloseChunk();
                return false;
            }
        }

        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "mesh: no MHDR chunk"))
        {
            SerializeHeader(Ar, Header);
            Ar.CloseChunk();
        }

        if(Ar.IsLoading() && !Ar.HasError())
        {
            std::copy(std::begin(Header.BoundsMin), std::end(Header.BoundsMin), InOutMesh.BoundsMin);
            std::copy(std::begin(Header.BoundsMax), std::end(Header.BoundsMax), InOutMesh.BoundsMax);

            /* A vertex is 8 floats on disk; an index 4 bytes. */
            if(CanHoldElements(Ar, uint64_t(Header.VertexCount), 8 * sizeof(float), "mesh: vertex count is past the end of the file")
               && CanHoldElements(Ar, uint64_t(Header.IndexCount), sizeof(uint32_t), "mesh: index count is past the end of the file"))
            {
                InOutMesh.Positions.assign(size_t(Header.VertexCount) * 3, 0.0f);
                InOutMesh.Normals.assign(size_t(Header.VertexCount) * 3, 0.0f);
                InOutMesh.UV0.assign(size_t(Header.VertexCount) * 2, 0.0f);
                InOutMesh.Indices.assign(Header.IndexCount, 0u);
            }
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_VERTICES, 1, "mesh: no MVTX chunk"))
        {
            SerializeFloats(Ar, InOutMesh.Positions);
            SerializeFloats(Ar, InOutMesh.Normals);
            SerializeFloats(Ar, InOutMesh.UV0);
            Ar.CloseChunk();
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_INDICES, 1, "mesh: no MIDX chunk"))
        {
            for(uint32_t& Index : InOutMesh.Indices) { Ar << Index; }
            Ar.CloseChunk();
        }

        if(!Ar.HasError() && OpenRequiredChunk(Ar, TAG_SECTIONS, 1, "mesh: no MSEC chunk"))
        {
            /* A section is at least two empty strings and two counts. */
            if(Ar.IsLoading() && CanHoldElements(Ar, Header.SectionCount, 4 * sizeof(uint32_t), "mesh: section count is past the end of the file"))
            {
                InOutMesh.Sections.assign(Header.SectionCount, MeshSection());
            }
            for(MeshSection& Section : InOutMesh.Sections)
            {
                Ar << Section.Name << Section.Material << Section.FirstIndex << Section.IndexCount;
            }
            Ar.CloseChunk();
        }

        /* Vertex colours: written only when the mesh has them; a file without
         * the chunk (every mesh before it existed) loads white. */
        const bool bColors = Ar.IsSaving() ? !InOutMesh.Colors.empty() : Ar.PeekChunkTag() == TAG_COLORS;
        if(!Ar.HasError() && bColors && OpenRequiredChunk(Ar, TAG_COLORS, 1, "mesh: bad MCOL chunk"))
        {
            if(Ar.IsLoading() && CanHoldElements(Ar, uint64_t(Header.VertexCount), 4 * sizeof(float), "mesh: colour count is past the end of the file"))
            {
                InOutMesh.Colors.assign(size_t(Header.VertexCount) * 4, 1.0f);
            }
            SerializeFloats(Ar, InOutMesh.Colors);
            Ar.CloseChunk();
        }

        /* Lightmap UVs, the same way: a file without them is not lightmapped. */
        const bool bUV1 = Ar.IsSaving() ? !InOutMesh.UV1.empty() : Ar.PeekChunkTag() == TAG_UV1;
        if(!Ar.HasError() && bUV1 && OpenRequiredChunk(Ar, TAG_UV1, 1, "mesh: bad MUV1 chunk"))
        {
            if(Ar.IsLoading() && CanHoldElements(Ar, uint64_t(Header.VertexCount), 2 * sizeof(float), "mesh: UV1 count is past the end of the file"))
            {
                InOutMesh.UV1.assign(size_t(Header.VertexCount) * 2, 0.0f);
            }
            SerializeFloats(Ar, InOutMesh.UV1);
            Ar.CloseChunk();
        }

        /* Skinning, the same way: a file without it is a static mesh. */
        const bool bSkin = Ar.IsSaving() ? InOutMesh.IsSkinned() : Ar.PeekChunkTag() == TAG_SKIN;
        if(!Ar.HasError() && bSkin && OpenRequiredChunk(Ar, TAG_SKIN, 1, "mesh: bad MSKN chunk"))
        {
            uint32_t BoneCount = uint32_t(InOutMesh.InverseBind.size());
            Ar << InOutMesh.Skeleton << BoneCount;
            if(Ar.IsLoading()
               && CanHoldElements(Ar, BoneCount, 12 * sizeof(float), "mesh: bone count is past the end of the file")
               && CanHoldElements(Ar, uint64_t(Header.VertexCount), 4 * (sizeof(uint16_t) + sizeof(float)),
                                  "mesh: skinned vertex count is past the end of the file"))
            {
                InOutMesh.InverseBind.assign(BoneCount, AnimMatrix());
                InOutMesh.Joints.assign(size_t(Header.VertexCount) * 4, 0);
                InOutMesh.Weights.assign(size_t(Header.VertexCount) * 4, 0.0f);
            }
            for(AnimMatrix& Matrix : InOutMesh.InverseBind)
            {
                for(float (&Row)[4] : Matrix.M) { for(float& Value : Row) { Ar << Value; } }
            }
            for(uint16_t& Joint : InOutMesh.Joints) { Ar << Joint; }
            SerializeFloats(Ar, InOutMesh.Weights);
            Ar.CloseChunk();
        }

        if(!Ar.HasError())
        {
            SerializeImportInfo(Ar, InOutMesh.ImportInfo);
        }

        Ar.CloseChunk();

        if(Ar.IsLoading() && !Ar.HasError())
        {
            if(const char* Defect = FindDefect(InOutMesh))
            {
                Ar.SetError(Defect);
            }
        }
        return !Ar.HasError();
    }

    bool LoadMeshFile(const std::string& InPath, MeshData& OutMesh)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        OutMesh = MeshData();
        if(!SerializeMesh(Ar, OutMesh))
        {
            RENDERCORE_LOG_WARN("cannot load mesh " + InPath)
            OutMesh = MeshData();
            return false;
        }
        return true;
    }

    bool LoadMeshHeader(const std::string& InPath, MeshHeader& OutHeader)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForReading({});
        if(!EngineCore::LoadArchiveFromFile(InPath, Ar))
        {
            return false;
        }
        if(!OpenRequiredChunk(Ar, TAG_MESH, 1, "not a mesh (no NMSH chunk)")) { return false; }
        if(OpenRequiredChunk(Ar, TAG_HEADER, 1, "mesh: no MHDR chunk"))
        {
            SerializeHeader(Ar, OutHeader);
            Ar.CloseChunk();
        }
        Ar.CloseChunk();
        return !Ar.HasError();
    }

#if WITH_EDITOR
    bool SaveMeshFile(const std::string& InPath, MeshData& InMesh)
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        if(!SerializeMesh(Ar, InMesh))
        {
            return false;
        }
        return EngineCore::SaveArchiveToFile(InPath, Ar);
    }
#endif

    // === MeshAssetCache =======================================================

    namespace
    {
        struct CacheState
        {
            std::mutex                                                       Lock;
            std::unordered_map<std::string, std::shared_ptr<const MeshData>> Meshes;   // null: failed
            std::unordered_map<std::string, std::shared_ptr<const MeshData>> Generated;   // Store's; Invalidate leaves them
            std::atomic<uint64_t>                                            Generation{0};
        };

        /* Function-local, so it exists before any static initialiser asks. */
        CacheState& GetCache()
        {
            static CacheState State;
            return State;
        }
    }

    std::shared_ptr<const MeshData> MeshAssetCache::Find(const std::string& InPath)
    {
        if(InPath.empty())
        {
            return nullptr;
        }

        CacheState& Cache = GetCache();
        {
            std::lock_guard<std::mutex> Guard(Cache.Lock);
            const auto Made = Cache.Generated.find(InPath);
            if(Made != Cache.Generated.end())
            {
                return Made->second;
            }
            const auto Found = Cache.Meshes.find(InPath);
            if(Found != Cache.Meshes.end())
            {
                return Found->second;
            }
        }

        /* Loaded outside the lock; if two threads race, the first stored wins. */
        auto Loaded = std::make_shared<MeshData>();
        std::shared_ptr<const MeshData> Result;
        if(LoadMeshFile(InPath, *Loaded))
        {
            Result = std::move(Loaded);
        }
        else
        {
            RENDERCORE_LOG_WARN("mesh '" + InPath + "' cannot be loaded; drawing a quad")
        }

        std::lock_guard<std::mutex> Guard(Cache.Lock);
        return Cache.Meshes.emplace(InPath, Result).first->second;
    }

    void MeshAssetCache::Store(const std::string& InPath, std::shared_ptr<const MeshData> InMesh)
    {
        if(InPath.empty())
        {
            return;
        }
        CacheState& Cache = GetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        if(InMesh)
        {
            Cache.Generated[InPath] = std::move(InMesh);
        }
        else
        {
            Cache.Generated.erase(InPath);
        }
    }

    void MeshAssetCache::Remove(const std::string& InPath)
    {
        Store(InPath, nullptr);
    }

    uint64_t MeshAssetCache::GetGeneration()
    {
        return GetCache().Generation.load(std::memory_order_relaxed);
    }

    void MeshAssetCache::Invalidate(const std::string& InPath)
    {
        CacheState& Cache = GetCache();
        std::lock_guard<std::mutex> Guard(Cache.Lock);
        Cache.Generation.fetch_add(1, std::memory_order_relaxed);
        if(InPath.empty())
        {
            Cache.Meshes.clear();
        }
        else
        {
            Cache.Meshes.erase(InPath);
        }
    }
}
