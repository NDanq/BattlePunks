#include "Public/RenderCore/MeshAsset.h"

#include <meshoptimizer.h>

namespace RenderCore
{
    namespace
    {
        /* How much the clusterizer favours a tight normal cone over a small
         * sphere: meshoptimizer's suggestion when cones are used at all. */
        constexpr float MESHLET_CONE_WEIGHT = 0.25f;
    }

    uint64_t MeshletFingerprint(const MeshData& InMesh)
    {
        /* FNV-1a over the vertex count and the indices. */
        uint64_t Hash = 14695981039346656037ull;
        const auto Mix = [&Hash](const uint32_t InValue)
        {
            for(int Byte = 0; Byte < 4; ++Byte)
            {
                Hash ^= (InValue >> (Byte * 8)) & 0xFFu;
                Hash *= 1099511628211ull;
            }
        };
        Mix(InMesh.GetVertexCount());
        for(const uint32_t Index : InMesh.Indices) { Mix(Index); }
        return Hash;
    }

    const char* FindMeshletDefect(const MeshData& InMesh)
    {
        if(!InMesh.HasMeshlets())
        {
            return InMesh.MeshletVertices.empty() && InMesh.MeshletTriangles.empty() ? nullptr : "meshlets: data without meshlets";
        }
        if(InMesh.MeshletSource != MeshletFingerprint(InMesh)) { return "meshlets: built from other indices"; }

        const uint32_t VertexCount = InMesh.GetVertexCount();
        for(const uint32_t Vertex : InMesh.MeshletVertices)
        {
            if(Vertex >= VertexCount) { return "meshlets: a vertex is past the mesh's last"; }
        }
        std::vector<uint64_t> Triangles(InMesh.Sections.size(), 0);
        for(const MeshMeshlet& Meshlet : InMesh.Meshlets)
        {
            if(Meshlet.Section >= InMesh.Sections.size()) { return "meshlets: a meshlet names a section past the last"; }
            if(Meshlet.VertexCount == 0 || Meshlet.VertexCount > MESHLET_MAX_VERTICES
               || Meshlet.TriangleCount == 0 || Meshlet.TriangleCount > MESHLET_MAX_TRIANGLES)
            {
                return "meshlets: a meshlet is empty or too big";
            }
            if(uint64_t(Meshlet.VertexOffset) + Meshlet.VertexCount > InMesh.MeshletVertices.size()
               || uint64_t(Meshlet.TriangleOffset) + uint64_t(Meshlet.TriangleCount) * 3 > InMesh.MeshletTriangles.size())
            {
                return "meshlets: a meshlet runs past its data";
            }
            for(uint32_t Corner = 0; Corner < Meshlet.TriangleCount * 3; ++Corner)
            {
                if(InMesh.MeshletTriangles[size_t(Meshlet.TriangleOffset) + Corner] >= Meshlet.VertexCount)
                {
                    return "meshlets: a triangle names a vertex past its meshlet's last";
                }
            }
            Triangles[Meshlet.Section] += Meshlet.TriangleCount;
        }
        /* Drawn instead of their sections, so they must cover them: as many
         * triangles each (BuildMeshlets keeps every one, degenerate or not). */
        for(size_t Section = 0; Section < InMesh.Sections.size(); ++Section)
        {
            if(Triangles[Section] * 3 != InMesh.Sections[Section].IndexCount) { return "meshlets: a section's triangle count differs"; }
        }
        return nullptr;
    }

    bool BuildMeshlets(MeshData& InOutMesh)
    {
        const uint32_t VertexCount = InOutMesh.GetVertexCount();
        if(InOutMesh.Indices.empty() || VertexCount == 0)
        {
            return false;
        }
        for(const uint32_t Index : InOutMesh.Indices)
        {
            if(Index >= VertexCount) { return false; }
        }

        std::vector<MeshMeshlet> Meshlets;
        std::vector<uint32_t>    MeshletVertices;
        std::vector<uint8_t>     MeshletTriangles;

        std::vector<meshopt_Meshlet> Built;
        std::vector<uint32_t>        BuiltVertices;
        std::vector<uint8_t>         BuiltTriangles;
        for(uint32_t SectionIndex = 0; SectionIndex < InOutMesh.Sections.size(); ++SectionIndex)
        {
            const MeshSection& Section = InOutMesh.Sections[SectionIndex];
            if(Section.IndexCount == 0)
            {
                continue;
            }
            if(Section.IndexCount % 3 != 0 || uint64_t(Section.FirstIndex) + Section.IndexCount > InOutMesh.Indices.size())
            {
                return false;
            }
            const uint32_t* Indices = InOutMesh.Indices.data() + Section.FirstIndex;
            const size_t    IndexCount = Section.IndexCount;

            const size_t MaxMeshlets = meshopt_buildMeshletsBound(IndexCount, MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES);
            Built.resize(MaxMeshlets);
            BuiltVertices.resize(MaxMeshlets * MESHLET_MAX_VERTICES);
            BuiltTriangles.resize(MaxMeshlets * MESHLET_MAX_TRIANGLES * 3);
            const size_t Count = meshopt_buildMeshlets(Built.data(), BuiltVertices.data(), BuiltTriangles.data(), Indices, IndexCount,
                                                       InOutMesh.Positions.data(), VertexCount, 3 * sizeof(float),
                                                       MESHLET_MAX_VERTICES, MESHLET_MAX_TRIANGLES, MESHLET_CONE_WEIGHT);

            for(size_t At = 0; At < Count; ++At)
            {
                const meshopt_Meshlet& Source = Built[At];
                meshopt_optimizeMeshlet(&BuiltVertices[Source.vertex_offset], &BuiltTriangles[Source.triangle_offset],
                                        Source.triangle_count, Source.vertex_count);
                const meshopt_Bounds Bounds = meshopt_computeMeshletBounds(&BuiltVertices[Source.vertex_offset],
                                                                           &BuiltTriangles[Source.triangle_offset], Source.triangle_count,
                                                                           InOutMesh.Positions.data(), VertexCount, 3 * sizeof(float));

                MeshMeshlet Meshlet;
                Meshlet.Section        = SectionIndex;
                Meshlet.VertexOffset   = uint32_t(MeshletVertices.size());
                Meshlet.VertexCount    = Source.vertex_count;
                Meshlet.TriangleOffset = uint32_t(MeshletTriangles.size());
                Meshlet.TriangleCount  = Source.triangle_count;
                for(int Axis = 0; Axis < 3; ++Axis)
                {
                    Meshlet.Center[Axis]   = Bounds.center[Axis];
                    Meshlet.ConeAxis[Axis] = Bounds.cone_axis[Axis];
                }
                Meshlet.Radius     = Bounds.radius;
                Meshlet.ConeCutoff = Bounds.cone_cutoff;

                /* The cone is the triangles' winding's; a mesh wound the other
                 * way round (the importer's mirror into engine space) has it
                 * pointing in. Turned to agree with the vertex normals, so
                 * "faces away" means what it shows. */
                if(InOutMesh.Normals.size() == size_t(VertexCount) * 3)
                {
                    double Agree = 0.0;
                    for(uint32_t Vertex = 0; Vertex < Source.vertex_count; ++Vertex)
                    {
                        const float* Normal = &InOutMesh.Normals[size_t(BuiltVertices[Source.vertex_offset + Vertex]) * 3];
                        Agree += double(Normal[0]) * Bounds.cone_axis[0] + double(Normal[1]) * Bounds.cone_axis[1]
                               + double(Normal[2]) * Bounds.cone_axis[2];
                    }
                    if(Agree < 0.0)
                    {
                        for(float& Axis : Meshlet.ConeAxis) { Axis = -Axis; }
                    }
                }
                Meshlets.push_back(Meshlet);

                MeshletVertices.insert(MeshletVertices.end(), BuiltVertices.begin() + Source.vertex_offset,
                                       BuiltVertices.begin() + Source.vertex_offset + Source.vertex_count);
                MeshletTriangles.insert(MeshletTriangles.end(), BuiltTriangles.begin() + Source.triangle_offset,
                                        BuiltTriangles.begin() + Source.triangle_offset + size_t(Source.triangle_count) * 3);
            }
        }
        if(Meshlets.empty())
        {
            return false;
        }

        InOutMesh.Meshlets         = std::move(Meshlets);
        InOutMesh.MeshletVertices  = std::move(MeshletVertices);
        InOutMesh.MeshletTriangles = std::move(MeshletTriangles);
        InOutMesh.MeshletSource    = MeshletFingerprint(InOutMesh);
        return true;
    }
}
