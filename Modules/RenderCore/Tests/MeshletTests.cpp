/*
 * RenderCoreMeshletTests -- meshlets without a window (MeshAsset.h):
 * BuildMeshlets over hand-made meshes must keep every section's triangles
 * exactly once, within the limits and inside their bounds; the MLET chunk
 * must round-trip, and a set that no longer fits its mesh must be dropped
 * without losing the mesh.
 */

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/MeshAsset.h"

using namespace RenderCore;
using EngineCore::Archive;

static int GChecks   = 0;
static int GFailures = 0;

static void Expect(const bool bCondition, const char* What)
{
    ++GChecks;
    if(!bCondition)
    {
        ++GFailures;
        std::printf("  FAIL  %s\n", What);
    }
}

/* A UV sphere of InRadius, InRings x InSegments quads with shared vertices:
 * the top half one section, the bottom half another. */
static MeshData MakeSphere(const float InRadius, const uint32_t InRings, const uint32_t InSegments)
{
    MeshData Mesh;
    for(uint32_t Ring = 0; Ring <= InRings; ++Ring)
    {
        const float Theta = 3.14159265f * float(Ring) / float(InRings);
        for(uint32_t Segment = 0; Segment <= InSegments; ++Segment)
        {
            const float Phi = 6.28318531f * float(Segment) / float(InSegments);
            const float X = std::sin(Theta) * std::cos(Phi), Y = std::cos(Theta), Z = std::sin(Theta) * std::sin(Phi);
            Mesh.Positions.insert(Mesh.Positions.end(), { X * InRadius, Y * InRadius, Z * InRadius });
            Mesh.Normals.insert(Mesh.Normals.end(), { X, Y, Z });
            Mesh.UV0.insert(Mesh.UV0.end(), { float(Segment) / float(InSegments), float(Ring) / float(InRings) });
        }
    }
    const uint32_t Row = InSegments + 1;
    for(uint32_t Half = 0; Half < 2; ++Half)
    {
        MeshSection Section;
        Section.Name       = Half == 0 ? "Top" : "Bottom";
        Section.FirstIndex = uint32_t(Mesh.Indices.size());
        for(uint32_t Ring = Half * InRings / 2; Ring < (Half + 1) * InRings / 2; ++Ring)
        {
            for(uint32_t Segment = 0; Segment < InSegments; ++Segment)
            {
                const uint32_t A = Ring * Row + Segment, B = A + 1, C = A + Row, D = C + 1;
                Mesh.Indices.insert(Mesh.Indices.end(), { A, C, B, B, C, D });
            }
        }
        Section.IndexCount = uint32_t(Mesh.Indices.size()) - Section.FirstIndex;
        Mesh.Sections.push_back(Section);
    }
    ComputeMeshBounds(Mesh);
    return Mesh;
}

/* A triangle as a sorted triple: the same triangle whatever its first corner. */
using Tri = std::array<uint32_t, 3>;

static Tri Sorted(uint32_t InA, uint32_t InB, uint32_t InC)
{
    Tri T{ InA, InB, InC };
    std::sort(T.begin(), T.end());
    return T;
}

/* True if every section's triangles are its meshlets' triangles, each once. */
static bool CoversSections(const MeshData& InMesh)
{
    for(uint32_t Section = 0; Section < InMesh.Sections.size(); ++Section)
    {
        const MeshSection& S = InMesh.Sections[Section];
        std::vector<Tri> Want, Have;
        for(uint32_t At = S.FirstIndex; At < S.FirstIndex + S.IndexCount; At += 3)
        {
            Want.push_back(Sorted(InMesh.Indices[At], InMesh.Indices[At + 1], InMesh.Indices[At + 2]));
        }
        for(const MeshMeshlet& M : InMesh.Meshlets)
        {
            if(M.Section != Section) { continue; }
            for(uint32_t T = 0; T < M.TriangleCount; ++T)
            {
                const uint8_t* Corner = &InMesh.MeshletTriangles[M.TriangleOffset + T * 3];
                const uint32_t* Vertex = &InMesh.MeshletVertices[M.VertexOffset];
                Have.push_back(Sorted(Vertex[Corner[0]], Vertex[Corner[1]], Vertex[Corner[2]]));
            }
        }
        std::sort(Want.begin(), Want.end());
        std::sort(Have.begin(), Have.end());
        if(Want != Have) { return false; }
    }
    return true;
}

/* True if every meshlet's sphere holds its vertices (to a hair). */
static bool SpheresHoldVertices(const MeshData& InMesh)
{
    for(const MeshMeshlet& M : InMesh.Meshlets)
    {
        for(uint32_t V = 0; V < M.VertexCount; ++V)
        {
            const float* P = &InMesh.Positions[size_t(InMesh.MeshletVertices[M.VertexOffset + V]) * 3];
            const float DX = P[0] - M.Center[0], DY = P[1] - M.Center[1], DZ = P[2] - M.Center[2];
            if(std::sqrt(DX * DX + DY * DY + DZ * DZ) > M.Radius * 1.001f + 1e-4f) { return false; }
        }
    }
    return true;
}

static bool LoadFrom(const Archive& InSaved, MeshData& OutMesh)
{
    Archive Reader = Archive::ForReading(InSaved.GetBytes());
    OutMesh = MeshData();
    return SerializeMesh(Reader, OutMesh);
}

static void TestBuild()
{
    std::printf("[build]\n");
    MeshData Sphere = MakeSphere(100.0f, 32, 48);
    const std::vector<uint32_t> Indices = Sphere.Indices;
    const std::vector<float>    Positions = Sphere.Positions;
    Expect(!Sphere.HasMeshlets(), "a mesh starts without meshlets");
    Expect(BuildMeshlets(Sphere), "a sphere builds meshlets");
    Expect(Sphere.Meshlets.size() >= 2 * 32 * 48 / MESHLET_MAX_TRIANGLES, "enough meshlets for its triangles");
    Expect(Sphere.Indices == Indices && Sphere.Positions == Positions, "the mesh's own vertices and indices are left alone");
    Expect(CoversSections(Sphere), "every section's triangles are in its meshlets exactly once");
    Expect(SpheresHoldVertices(Sphere), "every meshlet's sphere holds its vertices");
    Expect(FindMeshletDefect(Sphere) == nullptr, "and they pass the load checks");

    bool bWithin = true;
    for(const MeshMeshlet& M : Sphere.Meshlets)
    {
        bWithin = bWithin && M.VertexCount <= MESHLET_MAX_VERTICES && M.TriangleCount <= MESHLET_MAX_TRIANGLES
                  && M.VertexCount > 0 && M.TriangleCount > 0;
    }
    Expect(bWithin, "no meshlet passes the vertex or triangle limit");

    /* A sphere's patches are nearly flat: most cones are narrow enough to cull by. */
    size_t Narrow = 0;
    for(const MeshMeshlet& M : Sphere.Meshlets) { Narrow += M.ConeCutoff < 1.0f ? 1 : 0; }
    Expect(Narrow * 2 > Sphere.Meshlets.size(), "most of a sphere's meshlets have a cone to cull by");

    MeshData Again = MakeSphere(100.0f, 32, 48);
    BuildMeshlets(Again);
    Expect(BuildMeshlets(Again) && Again.Meshlets.size() == Sphere.Meshlets.size()
           && Again.MeshletVertices == Sphere.MeshletVertices && Again.MeshletTriangles == Sphere.MeshletTriangles,
           "building again replaces them, and the same mesh builds the same meshlets");

    /* A degenerate triangle is still drawn by its section, so it stays. */
    MeshData Degenerate = MakeSphere(10.0f, 4, 6);
    Degenerate.Indices.insert(Degenerate.Indices.end(), { 0, 0, 1 });
    Degenerate.Sections[1].IndexCount += 3;
    Expect(BuildMeshlets(Degenerate) && CoversSections(Degenerate) && FindMeshletDefect(Degenerate) == nullptr,
           "a degenerate triangle is kept");

    /* An empty section has no meshlets and needs none. */
    MeshData Empty = MakeSphere(10.0f, 4, 6);
    MeshSection None;
    None.FirstIndex = uint32_t(Empty.Indices.size());
    Empty.Sections.push_back(None);
    Expect(BuildMeshlets(Empty) && FindMeshletDefect(Empty) == nullptr, "an empty section is skipped");
}

static void TestRefusals()
{
    std::printf("[refusals]\n");
    MeshData Nothing;
    Expect(!BuildMeshlets(Nothing) && !Nothing.HasMeshlets(), "a mesh without triangles builds none");

    MeshData BadIndex = MakeSphere(10.0f, 4, 6);
    BadIndex.Indices[5] = 100000;
    Expect(!BuildMeshlets(BadIndex) && !BadIndex.HasMeshlets(), "an index past the last vertex is refused");

    MeshData BadSection = MakeSphere(10.0f, 4, 6);
    BadSection.Sections[1].IndexCount = 4;
    Expect(!BuildMeshlets(BadSection) && !BadSection.HasMeshlets(), "a section that is not whole triangles is refused");

    MeshData Kept = MakeSphere(10.0f, 4, 6);
    BuildMeshlets(Kept);
    const size_t Before = Kept.Meshlets.size();
    Kept.Indices[5] = 100000;
    Expect(!BuildMeshlets(Kept) && Kept.Meshlets.size() == Before, "a refused build leaves the last meshlets alone");
}

static void TestFile()
{
    std::printf("[file]\n");
    MeshData Sphere = MakeSphere(100.0f, 16, 24);
    BuildMeshlets(Sphere);

    Archive Writer = Archive::ForWriting();
    Expect(SerializeMesh(Writer, Sphere), "a mesh with meshlets saves");
    MeshData Loaded;
    Expect(LoadFrom(Writer, Loaded), "and loads back");
    bool bSame = Loaded.Meshlets.size() == Sphere.Meshlets.size() && Loaded.MeshletVertices == Sphere.MeshletVertices
                 && Loaded.MeshletTriangles == Sphere.MeshletTriangles && Loaded.MeshletSource == Sphere.MeshletSource;
    for(size_t At = 0; bSame && At < Loaded.Meshlets.size(); ++At)
    {
        const MeshMeshlet& A = Loaded.Meshlets[At];
        const MeshMeshlet& B = Sphere.Meshlets[At];
        bSame = A.Section == B.Section && A.VertexOffset == B.VertexOffset && A.VertexCount == B.VertexCount
                && A.TriangleOffset == B.TriangleOffset && A.TriangleCount == B.TriangleCount
                && std::memcmp(A.Center, B.Center, sizeof(A.Center)) == 0 && A.Radius == B.Radius
                && std::memcmp(A.ConeAxis, B.ConeAxis, sizeof(A.ConeAxis)) == 0 && A.ConeCutoff == B.ConeCutoff;
    }
    Expect(bSame, "with the same meshlets");

    MeshData Plain = MakeSphere(100.0f, 16, 24);
    Archive PlainWriter = Archive::ForWriting();
    MeshData PlainLoaded;
    Expect(SerializeMesh(PlainWriter, Plain) && LoadFrom(PlainWriter, PlainLoaded) && !PlainLoaded.HasMeshlets()
           && PlainWriter.GetBytes().size() < Writer.GetBytes().size(), "meshlets are optional, and cost nothing when absent");

    /* Indices changed after the build: the stale set is not written. */
    MeshData Stale = Sphere;
    std::swap(Stale.Indices[0], Stale.Indices[1]);
    Expect(FindMeshletDefect(Stale) != nullptr, "meshlets built from other indices are stale");
    Archive StaleWriter = Archive::ForWriting();
    MeshData StaleLoaded;
    Expect(SerializeMesh(StaleWriter, Stale) && LoadFrom(StaleWriter, StaleLoaded) && !StaleLoaded.HasMeshlets()
           && StaleLoaded.Indices == Stale.Indices, "a stale set is not saved, and the mesh still is");

    /* A file whose set no longer fits (its fingerprint altered on disk):
     * loads, without meshlets. */
    std::vector<uint8_t> Bytes = Writer.GetBytes();
    uint8_t Source[8];
    std::memcpy(Source, &Sphere.MeshletSource, sizeof(Source));
    const auto Found = std::search(Bytes.begin(), Bytes.end(), std::begin(Source), std::end(Source));
    Expect(Found != Bytes.end(), "the fingerprint is in the file");
    if(Found != Bytes.end())
    {
        *Found ^= 0x5A;
        Archive Reader = Archive::ForReading(Bytes);
        MeshData Tampered;
        Expect(SerializeMesh(Reader, Tampered) && !Tampered.HasMeshlets() && Tampered.MeshletVertices.empty()
               && Tampered.Indices == Sphere.Indices, "a set that does not fit is dropped on load, and the mesh kept");
    }

    /* A meshlet naming a vertex past its own: dropped the same way. */
    MeshData Broken = Sphere;
    Broken.MeshletTriangles[0] = uint8_t(Broken.Meshlets[0].VertexCount);
    Expect(FindMeshletDefect(Broken) != nullptr, "a corner past its meshlet's vertices is a defect");
    MeshData Short = Sphere;
    Short.Meshlets.pop_back();
    Expect(FindMeshletDefect(Short) != nullptr, "a set that leaves out triangles is a defect");
}

static void TestTiming()
{
    std::printf("[timing]\n");
    MeshData Big = MakeSphere(100.0f, 256, 256);
    const auto Start = std::chrono::steady_clock::now();
    BuildMeshlets(Big);
    const double Ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - Start).count();
    std::printf("  %u triangles into %zu meshlets in %.1f ms\n", Big.GetTriangleCount(), Big.Meshlets.size(), Ms);
    Expect(CoversSections(Big), "a 131k-triangle sphere is covered too");
}

int main()
{
    std::printf("RenderCore meshlet tests\n");
    TestBuild();
    TestRefusals();
    TestFile();
    TestTiming();
    std::printf("%d checks, %d failed\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
