/*
 * NimbleEditorImportTests -- glTF -> .nmmesh, images -> .nmtexture, Reimport.
 * The glTF is built inline so expected numbers sit beside their source.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/AssetImport.h"

using namespace EditorContent;

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

static bool Near(const float A, const float B) { return std::fabs(A - B) < 1e-4f; }

/* Reads a written asset back the way the editor's Reimport does. */
template<typename TData, typename TSerialize>
static bool ReadAsset(const QString& InPath, TData& OutData, TSerialize InSerialize)
{
    QFile File(InPath);
    if(!File.open(QIODevice::ReadOnly)) { return false; }
    const QByteArray Bytes = File.readAll();
    EngineCore::Archive Ar = EngineCore::Archive::ForReading(std::vector<uint8_t>(Bytes.begin(), Bytes.end()));
    return InSerialize(Ar, OutData);
}

static bool ReadMesh(const QString& InPath, RenderCore::MeshData& OutMesh)
{
    return ReadAsset(InPath, OutMesh, &RenderCore::SerializeMesh);
}

static bool ReadTexture(const QString& InPath, RenderCore::TextureData& OutTexture)
{
    return ReadAsset(InPath, OutTexture, &RenderCore::SerializeTexture);
}

/*
 * Scene.gltf, beside Checker.png:
 *   node "Moved"  (translation 1,2,3)  -> mesh "Box", two primitives:
 *       0: a triangle at z=0, indexed, normals +Z, material "Red"
 *       1: a triangle at z=1, no indices, no normals, COLOR_0 as
 *          normalised bytes: red, green, half-transparent blue
 *   node "Mirror" (scale -1,1,1)       -> mesh "Tri": primitive 0's data again
 */
static void WriteScene(const QDir& InFolder)
{
    const float PosA[9]  = { 0, 0, 0,   1, 0, 0,   0, 1, 0 };
    const float NrmA[9]  = { 0, 0, 1,   0, 0, 1,   0, 0, 1 };
    const float UVA[6]   = { 0, 0,      1, 0,      0, 1 };
    const uint16_t Idx[4] = { 0, 1, 2, 0 };   // the 4th is padding, to keep the next view aligned
    const float PosB[9]  = { 0, 0, 1,   1, 0, 1,   0, 1, 1 };
    const uint8_t ColB[12] = { 255, 0, 0, 255,   0, 255, 0, 255,   0, 0, 255, 128 };

    QByteArray Buffer;
    Buffer.append(reinterpret_cast<const char*>(PosA), sizeof(PosA));   //   0..36
    Buffer.append(reinterpret_cast<const char*>(NrmA), sizeof(NrmA));   //  36..72
    Buffer.append(reinterpret_cast<const char*>(UVA),  sizeof(UVA));    //  72..96
    Buffer.append(reinterpret_cast<const char*>(Idx),  sizeof(Idx));    //  96..104 (6 used)
    Buffer.append(reinterpret_cast<const char*>(PosB), sizeof(PosB));   // 104..140
    Buffer.append(reinterpret_cast<const char*>(ColB), sizeof(ColB));   // 140..152

    const QString Json = QStringLiteral(R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0, 1] } ],
  "nodes": [
    { "name": "Moved",  "mesh": 0, "translation": [1, 2, 3] },
    { "name": "Mirror", "mesh": 1, "scale": [-1, 1, 1] }
  ],
  "meshes": [
    { "name": "Box", "primitives": [
        { "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3, "material": 0 },
        { "attributes": { "POSITION": 4, "COLOR_0": 5 } } ] },
    { "name": "Tri", "primitives": [
        { "attributes": { "POSITION": 0, "NORMAL": 1, "TEXCOORD_0": 2 }, "indices": 3 } ] }
  ],
  "materials": [ { "name": "Red" } ],
  "images": [ { "uri": "Checker.png" } ],
  "textures": [ { "source": 0 } ],
  "buffers": [ { "byteLength": %1, "uri": "data:application/octet-stream;base64,%2" } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,   "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36,  "byteLength": 36 },
    { "buffer": 0, "byteOffset": 72,  "byteLength": 24 },
    { "buffer": 0, "byteOffset": 96,  "byteLength": 6 },
    { "buffer": 0, "byteOffset": 104, "byteLength": 36 },
    { "buffer": 0, "byteOffset": 140, "byteLength": 12 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [1,1,0] },
    { "bufferView": 1, "componentType": 5126, "count": 3, "type": "VEC3" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC2" },
    { "bufferView": 3, "componentType": 5123, "count": 3, "type": "SCALAR" },
    { "bufferView": 4, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,1], "max": [1,1,1] },
    { "bufferView": 5, "componentType": 5121, "normalized": true, "count": 3, "type": "VEC4" }
  ]
})").arg(Buffer.size()).arg(QString::fromLatin1(Buffer.toBase64()));

    QFile File(InFolder.filePath(QStringLiteral("Scene.gltf")));
    File.open(QIODevice::WriteOnly);
    File.write(Json.toUtf8());
    File.close();

    /* 8 x 4 blue, the top-right 2x2 block green -- one pixel of the 4 x 2 it
     * scales down to, so a vertical flip shows. */
    QImage Checker(8, 4, QImage::Format_RGBA8888);
    Checker.fill(QColor(0, 0, 255));
    for(int Y = 0; Y < 2; ++Y)
    {
        for(int X = 6; X < 8; ++X) { Checker.setPixelColor(X, Y, QColor(0, 255, 0)); }
    }
    Checker.save(InFolder.filePath(QStringLiteral("Checker.png")));
}

static void TestClassify()
{
    std::printf("[classify]\n");
    Expect(ClassifySource(QStringLiteral("a/b/Crate.GLB")) == EImportKind::Mesh, ".glb is a mesh, case-insensitively");
    Expect(ClassifySource(QStringLiteral("Crate.gltf")) == EImportKind::Mesh, ".gltf is a mesh");
    Expect(ClassifySource(QStringLiteral("Bricks.png")) == EImportKind::Texture, ".png is a texture");
    Expect(ClassifySource(QStringLiteral("Notes.txt")) == EImportKind::None, ".txt is not importable");
    Expect(GetImportFileFilter().contains(QStringLiteral("*.glb")), "the file filter offers .glb");

    MeshImportOptions Mesh;
    Mesh.Scale = 2.5f;
    Mesh.bCombineMeshes = false;
    const MeshImportOptions MeshBack = MeshImportOptions::FromSettings(Mesh.ToSettings());
    Expect(Near(MeshBack.Scale, 2.5f) && !MeshBack.bCombineMeshes && MeshBack.bConvertCoordinates,
           "mesh options survive their settings text");

    TextureImportOptions Texture;
    Texture.bSRGB = false;
    Texture.Wrap  = RenderCore::ETextureWrap::Clamp;
    Texture.MaxDimension = 512;
    const TextureImportOptions TextureBack = TextureImportOptions::FromSettings(Texture.ToSettings());
    Expect(!TextureBack.bSRGB && TextureBack.Wrap == RenderCore::ETextureWrap::Clamp && TextureBack.MaxDimension == 512,
           "texture options survive their settings text");
    Texture.MipGen      = RenderCore::ETextureMipGen::Kaiser;
    Texture.Compression = RenderCore::ETextureCompression::BC5;
    const TextureImportOptions MipsBack = TextureImportOptions::FromSettings(Texture.ToSettings());
    Expect(MipsBack.MipGen == RenderCore::ETextureMipGen::Kaiser && MipsBack.Compression == RenderCore::ETextureCompression::BC5,
           "mip generation and compression too");
    const TextureImportOptions Older = TextureImportOptions::FromSettings(QStringLiteral("SRGB=1\nFilter=1\nWrap=0\n"));
    Expect(Older.MipGen == RenderCore::ETextureMipGen::None && Older.Compression == RenderCore::ETextureCompression::None,
           "settings from before mips read as none: a reimport makes what it made");
    Expect(TextureImportOptions::IsDataMapName(QStringLiteral("brick-wall_normal-ogl.png"))
           && TextureImportOptions::IsDataMapName(QStringLiteral("Metal_Roughness.jpg"))
           && TextureImportOptions::IsDataMapName(QStringLiteral("rockAO.png"))
           && TextureImportOptions::IsDataMapName(QStringLiteral("T_Crate_ORM.tga"))
           && !TextureImportOptions::IsDataMapName(QStringLiteral("brick-wall_albedo.png"))
           && !TextureImportOptions::IsDataMapName(QStringLiteral("Bricks.png"))
           && !TextureImportOptions::IsDataMapName(QStringLiteral("Normandy.png")), "data maps are told by whole words of the name");
    TextureImportOptions Detect;
    Expect(!Detect.ResolvedFor(QStringLiteral("wall_normal.png")).bSRGB && Detect.ResolvedFor(QStringLiteral("wall_albedo.png")).bSRGB,
           "a data map imports linear, a colour map sRGB");
    Expect(!Detect.ResolvedFor(QStringLiteral("wall_normal.png")).bDetectDataMaps, "and the recorded settings keep the decision");
}

static void TestMeshImport(const QDir& InRoot)
{
    std::printf("[mesh import]\n");

    MeshImportOptions Options;
    Options.Scale = 1.0f;
    TextureImportOptions TextureOptions;
    const QString Source = InRoot.filePath(QStringLiteral("Source/Scene.gltf"));
    const QString Content = InRoot.filePath(QStringLiteral("Content"));

    const ImportResult Result = ImportMesh(Source, Content, QStringLiteral("Scene"), Options, TextureOptions, InRoot.absolutePath());
    Expect(Result.bOK, "a glTF imports");
    if(!Result.bOK) { std::printf("        %s\n", qPrintable(Result.Message)); return; }
    Expect(Result.Written.size() == 2, "one combined mesh and its one texture are written");
    Expect(Result.Images.size() == 1 && Result.Images[0].SourcePath.endsWith(QStringLiteral("Checker.png")),
           "the referenced image is found beside the glTF");

    RenderCore::MeshData Mesh;
    Expect(ReadMesh(QDir(Content).filePath(QStringLiteral("Scene.nmmesh")), Mesh), "the .nmmesh loads");
    Expect(Mesh.GetVertexCount() == 9 && Mesh.Indices.size() == 9, "every primitive's vertices and indices, combined");
    Expect(Mesh.Sections.size() == 3, "a section per primitive");
    if(Mesh.Sections.size() == 3 && Mesh.GetVertexCount() == 9)
    {
        Expect(Mesh.Sections[0].Name == "Box.0" && Mesh.Sections[0].Material == "Red" && Mesh.Sections[1].Material.empty(),
               "sections are named after their mesh, with the glTF material name");
        Expect(Near(Mesh.Positions[0], 1.0f) && Near(Mesh.Positions[1], 2.0f) && Near(Mesh.Positions[2], -3.0f),
               "the node's translation is baked in, then z flips into the engine's left-handed space");
        Expect(Near(Mesh.Normals[2], -1.0f), "a +Z normal faces -Z in engine space");
        Expect(Mesh.Colors.size() == 9 * 4, "one primitive's COLOR_0 gives the whole mesh colours");
        if(Mesh.Colors.size() == 9 * 4)
        {
            Expect(Near(Mesh.Colors[0], 1.0f) && Near(Mesh.Colors[1], 1.0f) && Near(Mesh.Colors[3], 1.0f),
                   "vertices before the coloured primitive are white");
            Expect(Near(Mesh.Colors[3 * 4 + 0], 1.0f) && Near(Mesh.Colors[3 * 4 + 1], 0.0f) && Near(Mesh.Colors[3 * 4 + 3], 1.0f),
                   "normalised byte colours read as 0-1 floats");
            Expect(std::fabs(Mesh.Colors[5 * 4 + 3] - 128.0f / 255.0f) < 1e-3f, "alpha comes through");
            Expect(Near(Mesh.Colors[6 * 4 + 0], 1.0f) && Near(Mesh.Colors[8 * 4 + 3], 1.0f),
                   "a later primitive without colours is white");
        }
        Expect(Near(Mesh.Normals[3 * 3 + 2], -1.0f) && Near(Mesh.Normals[3 * 3 + 0], 0.0f),
               "a primitive with no normals gets generated ones, facing the same way");
        Expect(Mesh.Indices[0] == 0 && Mesh.Indices[1] == 2 && Mesh.Indices[2] == 1,
               "the axis conversion is a mirror, so it swaps a triangle's winding");
        Expect(Mesh.Indices[6] == 6 && Mesh.Indices[7] == 7 && Mesh.Indices[8] == 8,
               "a mirroring node transform and the conversion cancel: its winding is kept");
        Expect(Near(Mesh.BoundsMin[0], -1.0f) && Near(Mesh.BoundsMax[0], 2.0f), "bounds span both nodes");
        Expect(Near(Mesh.UV0[2], 1.0f), "UVs carry over");
    }

    /* No TEXCOORD_1 in the file, so lightmap UVs are generated: one per
     * vertex, in 0-1, and no texel centre (at the smallest lightmap
     * resolution, 32) inside two triangles. */
    Expect(Mesh.UV1.size() == size_t(Mesh.GetVertexCount()) * 2, "lightmap UVs are generated for every vertex");
    if(Mesh.UV1.size() == size_t(Mesh.GetVertexCount()) * 2)
    {
        bool bInRange = true;
        for(const float Each : Mesh.UV1) { bInRange &= Each >= 0.0f && Each <= 1.0f; }
        Expect(bInRange, "lightmap UVs are in 0-1");

        constexpr int Size = 32;
        std::vector<int> Covered(Size * Size, 0);
        for(size_t At = 0; At + 2 < Mesh.Indices.size(); At += 3)
        {
            const float* A = &Mesh.UV1[size_t(Mesh.Indices[At]) * 2];
            const float* B = &Mesh.UV1[size_t(Mesh.Indices[At + 1]) * 2];
            const float* C = &Mesh.UV1[size_t(Mesh.Indices[At + 2]) * 2];
            const auto Edge = [](const float* P, const float* Q, const float X, const float Y)
            {
                return (Q[0] - P[0]) * (Y - P[1]) - (Q[1] - P[1]) * (X - P[0]);
            };
            for(int Y = 0; Y < Size; ++Y)
            {
                for(int X = 0; X < Size; ++X)
                {
                    const float U = (float(X) + 0.5f) / Size, V = (float(Y) + 0.5f) / Size;
                    const float E0 = Edge(A, B, U, V), E1 = Edge(B, C, U, V), E2 = Edge(C, A, U, V);
                    if((E0 >= 0.0f && E1 >= 0.0f && E2 >= 0.0f) || (E0 <= 0.0f && E1 <= 0.0f && E2 <= 0.0f)) { ++Covered[size_t(Y * Size + X)]; }
                }
            }
        }
        Expect(*std::max_element(Covered.begin(), Covered.end()) <= 1, "lightmap UV charts do not overlap");
    }
    Expect(Mesh.ImportInfo.SourcePath == "Source/Scene.gltf", "the source is recorded relative to the project");

    RenderCore::TextureData Texture;
    Expect(ReadTexture(QDir(Content).filePath(QStringLiteral("Checker.nmtexture")), Texture)
           && Texture.Pixels.GetWidth() == 8, "the referenced image becomes a .nmtexture");

    /* Split: a file per glTF mesh. */
    Options.bCombineMeshes  = false;
    Options.bImportTextures = false;
    const ImportResult Split = ImportMesh(Source, Content, QStringLiteral("Split"), Options, TextureOptions, InRoot.absolutePath());
    Expect(Split.bOK && Split.Written.size() == 2, "split, a file per mesh");
    RenderCore::MeshData Tri;
    Expect(ReadMesh(QDir(Content).filePath(QStringLiteral("Split_Tri.nmmesh")), Tri) && Tri.GetTriangleCount() == 1,
           "named <base>_<mesh>");

    /* Reimport: the same file, from its own record; scale 100 is the default,
     * so prove the recorded scale (1) is the one used. */
    const ImportResult Again = Reimport(QDir(Content).filePath(QStringLiteral("Split_Tri.nmmesh")), InRoot.absolutePath());
    Expect(Again.bOK, "a split mesh reimports");
    RenderCore::MeshData TriAgain;
    Expect(ReadMesh(QDir(Content).filePath(QStringLiteral("Split_Tri.nmmesh")), TriAgain)
           && TriAgain.Positions == Tri.Positions, "with the options it was imported with");

    const ImportResult TextureAgain = Reimport(QDir(Content).filePath(QStringLiteral("Checker.nmtexture")), InRoot.absolutePath());
    Expect(TextureAgain.bOK, "a texture reimports");
}

static void TestTextureImport(const QDir& InRoot)
{
    std::printf("[texture import]\n");

    TextureImportOptions Options;
    Options.MaxDimension    = 4;
    Options.bFlipVertically = true;
    Options.bSRGB           = false;
    Options.Filter          = RenderCore::ETextureFilter::Nearest;
    const QString Content = InRoot.filePath(QStringLiteral("Content"));

    const ImportResult Result = ImportTexture(InRoot.filePath(QStringLiteral("Source/Checker.png")), Content,
                                              QStringLiteral("Small"), Options, InRoot.absolutePath());
    Expect(Result.bOK, "a PNG imports");
    Expect(Result.Warnings.size() == 1, "and says it was scaled down");

    RenderCore::TextureData Texture;
    Expect(ReadTexture(QDir(Content).filePath(QStringLiteral("Small.nmtexture")), Texture), "the .nmtexture loads");
    Expect(Texture.Pixels.GetWidth() == 4 && Texture.Pixels.GetHeight() == 2, "scaled to fit 4, keeping its shape");
    Expect(!Texture.bSRGB && Texture.Filter == RenderCore::ETextureFilter::Nearest, "with its sampling options");
    Expect(Texture.MipGen == RenderCore::ETextureMipGen::Box && Texture.GetMipCount() == 3,
           "and Box mips by default: 4 x 2, 2 x 1, 1 x 1");
    if(Texture.Pixels.GetWidth() == 4)
    {
        /* Flipped: the top-right green pixel is now in the bottom row. */
        const uint8_t* Top    = Texture.Pixels.GetScanline(0);
        const uint8_t* Bottom = Texture.Pixels.GetScanline(1);
        Expect(Bottom[3 * 4 + 1] > 200 && Bottom[3 * 4 + 2] < 50 && Top[3 * 4 + 2] > 200,
               "flipped vertically, rows in RGBA order");
    }

    QFile Garbage(InRoot.filePath(QStringLiteral("Source/Broken.gltf")));
    Garbage.open(QIODevice::WriteOnly);
    Garbage.write("this is not json");
    Garbage.close();
    const ImportResult Broken = ImportMesh(Garbage.fileName(), Content, QStringLiteral("Broken"), MeshImportOptions(),
                                           TextureImportOptions(), InRoot.absolutePath());
    Expect(!Broken.bOK && !Broken.Message.isEmpty(), "a broken glTF fails, saying why");
    Expect(!QFile::exists(QDir(Content).filePath(QStringLiteral("Broken.nmmesh"))), "and writes nothing");

    const ImportResult Missing = ImportTexture(InRoot.filePath(QStringLiteral("Source/None.png")), Content,
                                               QStringLiteral("None"), Options, InRoot.absolutePath());
    Expect(!Missing.bOK, "a missing image fails");
}

int main(int argc, char** argv)
{
    QCoreApplication App(argc, argv);   // image format plugins load through it
    std::printf("Editor import tests\n");

    QTemporaryDir Temp;
    const QDir Root(Temp.path());
    Root.mkpath(QStringLiteral("Source"));
    WriteScene(QDir(Root.filePath(QStringLiteral("Source"))));

    TestClassify();
    TestMeshImport(Root);
    TestTextureImport(Root);

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
