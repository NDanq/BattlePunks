#if WITH_EDITOR

#include "Private/Editor/Content/LightmapUVs.h"

#include <algorithm>
#include <cmath>
#include <memory>

#include "xatlas.h"

namespace EditorContent
{
    namespace
    {
        /* The atlas xatlas packs for: charts land a PACK_PADDING-texel gutter
         * apart at this size, which is one texel at MIN_LIGHTMAP_RESOLUTION. */
        constexpr uint32_t PACK_RESOLUTION = 256;
        constexpr uint32_t PACK_PADDING    = PACK_RESOLUTION / MIN_LIGHTMAP_RESOLUTION;

        double SurfaceArea(const RenderCore::MeshData& InMesh)
        {
            double Area = 0.0;
            for(size_t At = 0; At + 2 < InMesh.Indices.size(); At += 3)
            {
                const float* A = &InMesh.Positions[size_t(InMesh.Indices[At]) * 3];
                const float* B = &InMesh.Positions[size_t(InMesh.Indices[At + 1]) * 3];
                const float* C = &InMesh.Positions[size_t(InMesh.Indices[At + 2]) * 3];
                const double AB[3] = { B[0] - A[0], B[1] - A[1], B[2] - A[2] };
                const double AC[3] = { C[0] - A[0], C[1] - A[1], C[2] - A[2] };
                const double X = AB[1] * AC[2] - AB[2] * AC[1];
                const double Y = AB[2] * AC[0] - AB[0] * AC[2];
                const double Z = AB[0] * AC[1] - AB[1] * AC[0];
                Area += 0.5 * std::sqrt(X * X + Y * Y + Z * Z);
            }
            return Area;
        }

        struct AtlasDeleter { void operator()(xatlas::Atlas* InAtlas) const { xatlas::Destroy(InAtlas); } };
    }

    bool GenerateLightmapUVs(RenderCore::MeshData& InOutMesh, std::string& OutError)
    {
        const uint32_t VertexCount = InOutMesh.GetVertexCount();
        const double   Area        = SurfaceArea(InOutMesh);
        if(VertexCount == 0 || InOutMesh.Indices.empty() || Area <= 0.0)
        {
            OutError = "the mesh has no surface to chart";
            return false;
        }

        std::unique_ptr<xatlas::Atlas, AtlasDeleter> Atlas(xatlas::Create());

        /* Sections are materials: a chart never spans two. */
        std::vector<uint32_t> FaceMaterials(InOutMesh.Indices.size() / 3, 0u);
        for(size_t Section = 0; Section < InOutMesh.Sections.size(); ++Section)
        {
            const RenderCore::MeshSection& Each = InOutMesh.Sections[Section];
            for(uint32_t Face = Each.FirstIndex / 3; Face < (Each.FirstIndex + Each.IndexCount) / 3 && Face < FaceMaterials.size(); ++Face)
            {
                FaceMaterials[Face] = uint32_t(Section);
            }
        }

        xatlas::MeshDecl Decl;
        Decl.vertexPositionData   = InOutMesh.Positions.data();
        Decl.vertexPositionStride = sizeof(float) * 3;
        Decl.vertexNormalData     = InOutMesh.Normals.data();
        Decl.vertexNormalStride   = sizeof(float) * 3;
        Decl.vertexCount          = VertexCount;
        Decl.indexData            = InOutMesh.Indices.data();
        Decl.indexCount           = uint32_t(InOutMesh.Indices.size());
        Decl.indexFormat          = xatlas::IndexFormat::UInt32;
        Decl.faceMaterialData     = FaceMaterials.data();
        const xatlas::AddMeshError Added = xatlas::AddMesh(Atlas.get(), Decl);
        if(Added != xatlas::AddMeshError::Success)
        {
            OutError = std::string("xatlas: ") + xatlas::StringForEnum(Added);
            return false;
        }

        /* Texels per unit for one PACK_RESOLUTION atlas, allowing for the
         * space packing leaves; a single atlas, since resolution stays 0. */
        xatlas::PackOptions Pack;
        Pack.padding       = PACK_PADDING;
        Pack.bilinear      = true;
        Pack.texelsPerUnit = float(PACK_RESOLUTION * 0.7 / std::sqrt(Area));
        xatlas::Generate(Atlas.get(), xatlas::ChartOptions(), Pack);

        if(Atlas->meshCount != 1 || Atlas->width == 0 || Atlas->height == 0)
        {
            OutError = "xatlas made no atlas";
            return false;
        }
        const xatlas::Mesh& Out = Atlas->meshes[0];
        if(Out.indexCount != InOutMesh.Indices.size())
        {
            OutError = "xatlas changed the triangle count";
            return false;
        }

        /* Square, so a texel is as tall as it is wide in any lightmap. */
        const float Extent = float(std::max(Atlas->width, Atlas->height));

        /* Each original vertex keeps its index (its first copy in xatlas's
         * output); the copies a seam needs go on the end. */
        std::vector<uint32_t> Remap(Out.vertexCount);
        std::vector<uint32_t> Sources(VertexCount);
        std::vector<bool>     bPlaced(VertexCount, false);
        for(uint32_t Vertex = 0; Vertex < VertexCount; ++Vertex) { Sources[Vertex] = Vertex; }
        for(uint32_t Vertex = 0; Vertex < Out.vertexCount; ++Vertex)
        {
            const uint32_t Original = Out.vertexArray[Vertex].xref;
            if(Original < VertexCount && !bPlaced[Original])
            {
                bPlaced[Original] = true;
                Remap[Vertex]     = Original;
            }
            else
            {
                Remap[Vertex] = uint32_t(Sources.size());
                Sources.push_back(Original);
            }
        }

        RenderCore::MeshData Split = InOutMesh;
        const auto Gather = [&Sources](const auto& InSource, auto& OutTarget, const size_t InWidth)
        {
            if(InSource.empty()) { return; }
            OutTarget.assign(Sources.size() * InWidth, {});
            for(size_t Vertex = 0; Vertex < Sources.size(); ++Vertex)
            {
                std::copy_n(InSource.begin() + std::ptrdiff_t(size_t(Sources[Vertex]) * InWidth), InWidth,
                            OutTarget.begin() + std::ptrdiff_t(Vertex * InWidth));
            }
        };
        Gather(InOutMesh.Positions, Split.Positions, 3);
        Gather(InOutMesh.Normals,   Split.Normals,   3);
        Gather(InOutMesh.UV0,       Split.UV0,       2);
        Gather(InOutMesh.Colors,    Split.Colors,    4);
        Gather(InOutMesh.Tangents,  Split.Tangents,  4);
        Gather(InOutMesh.Joints,   Split.Joints,    4);   // a skinned mesh's copies keep their bones
        Gather(InOutMesh.Weights,   Split.Weights,   4);

        Split.UV1.assign(Sources.size() * 2, 0.0f);
        for(uint32_t Vertex = 0; Vertex < Out.vertexCount; ++Vertex)
        {
            Split.UV1[size_t(Remap[Vertex]) * 2]     = Out.vertexArray[Vertex].uv[0] / Extent;
            Split.UV1[size_t(Remap[Vertex]) * 2 + 1] = Out.vertexArray[Vertex].uv[1] / Extent;
        }
        Split.Indices.resize(Out.indexCount);
        for(uint32_t At = 0; At < Out.indexCount; ++At) { Split.Indices[At] = Remap[Out.indexArray[At]]; }

        InOutMesh = std::move(Split);
        return true;
    }
}

#endif // WITH_EDITOR
