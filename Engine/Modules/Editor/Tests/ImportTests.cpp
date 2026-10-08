/*
 * NimbleEditorImportTests -- glTF -> .nmmesh, images -> .nmtexture, Reimport.
 * The glTF is built inline so expected numbers sit beside their source.
 */

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

#include <QBuffer>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>

#include "Public/Core/CoreArchive.h"
#include "Public/RenderCore/AnimClipAsset.h"
#include "Public/RenderCore/MeshAsset.h"
#include "Public/RenderCore/SkeletonAsset.h"
#include "Public/RenderCore/TextureAsset.h"
#include "Private/Editor/Content/AssetImport.h"
#include "Private/Editor/Content/LightmapUVs.h"
#if WITH_AUDIO
#include "Public/Audio/SoundAsset.h"
#endif

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

/*
 * Skinned.gltf: a two-bone arm and one animation.
 *   node "Root" (at the origin)  -> child "Arm" (translation 1,0,0)
 *   node "Body" (translation 5,5,5 -- ignored: a skinned mesh is in bind
 *       space) -> mesh "Arm", skin 0: joints [Root, Arm], inverse binds
 *       identity and translation(-1,0,0)
 *   one triangle: (0,0,0) on Root, (2,0,0) on Arm, (0,1,0) half each
 *   animation "Bend": Arm turns 90 degrees about Z over one second
 */
/* A 2 x 2 PNG, for an embedded image with no name. */
static QByteArray TinyPng()
{
    QImage Image(2, 2, QImage::Format_RGBA8888);
    Image.fill(QColor(200, 120, 40));
    QByteArray Bytes;
    QBuffer Out(&Bytes);
    Out.open(QIODevice::WriteOnly);
    Image.save(&Out, "PNG");
    return Bytes;
}

static void WriteSkinnedScene(const QDir& InFolder)
{
    const float    Pos[9]     = { 0, 0, 0,   2, 0, 0,   0, 1, 0 };
    const uint8_t  Joints[12] = { 0, 0, 0, 0,   1, 0, 0, 0,   0, 1, 0, 0 };
    const float    Weights[12] = { 1, 0, 0, 0,   1, 0, 0, 0,   0.5f, 0.5f, 0, 0 };
    const float    InvBind[32] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1,
                                   1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0, -1, 0, 0, 1 };
    const float    Times[2]   = { 0, 1 };
    const float    S = std::sqrt(0.5f);
    const float    Rot[8]     = { 0, 0, 0, 1,   0, 0, S, S };

    QByteArray Buffer;
    Buffer.append(reinterpret_cast<const char*>(Pos),     sizeof(Pos));       //   0..36
    Buffer.append(reinterpret_cast<const char*>(Joints),  sizeof(Joints));    //  36..48
    Buffer.append(reinterpret_cast<const char*>(Weights), sizeof(Weights));   //  48..96
    Buffer.append(reinterpret_cast<const char*>(InvBind), sizeof(InvBind));   //  96..224
    Buffer.append(reinterpret_cast<const char*>(Times),   sizeof(Times));     // 224..232
    Buffer.append(reinterpret_cast<const char*>(Rot),     sizeof(Rot));       // 232..264

    const QString Json = QStringLiteral(R"({
  "asset": { "version": "2.0" },
  "scene": 0,
  "scenes": [ { "nodes": [0, 2] } ],
  "nodes": [
    { "name": "Root", "children": [1] },
    { "name": "Arm", "translation": [1, 0, 0] },
    { "name": "Body", "mesh": 0, "skin": 0, "translation": [5, 5, 5] }
  ],
  "skins": [ { "joints": [0, 1], "inverseBindMatrices": 3 } ],
  "images": [ { "uri": "data:image/png;base64,%3" } ],
  "meshes": [ { "name": "Arm", "primitives": [ { "attributes": { "POSITION": 0, "JOINTS_0": 1, "WEIGHTS_0": 2 } } ] } ],
  "animations": [ { "name": "Bend",
      "samplers": [ { "input": 4, "output": 5, "interpolation": "LINEAR" } ],
      "channels": [ { "sampler": 0, "target": { "node": 1, "path": "rotation" } } ] } ],
  "buffers": [ { "byteLength": %1, "uri": "data:application/octet-stream;base64,%2" } ],
  "bufferViews": [
    { "buffer": 0, "byteOffset": 0,   "byteLength": 36 },
    { "buffer": 0, "byteOffset": 36,  "byteLength": 12 },
    { "buffer": 0, "byteOffset": 48,  "byteLength": 48 },
    { "buffer": 0, "byteOffset": 96,  "byteLength": 128 },
    { "buffer": 0, "byteOffset": 224, "byteLength": 8 },
    { "buffer": 0, "byteOffset": 232, "byteLength": 32 }
  ],
  "accessors": [
    { "bufferView": 0, "componentType": 5126, "count": 3, "type": "VEC3", "min": [0,0,0], "max": [2,1,0] },
    { "bufferView": 1, "componentType": 5121, "count": 3, "type": "VEC4" },
    { "bufferView": 2, "componentType": 5126, "count": 3, "type": "VEC4" },
    { "bufferView": 3, "componentType": 5126, "count": 2, "type": "MAT4" },
    { "bufferView": 4, "componentType": 5126, "count": 2, "type": "SCALAR", "min": [0], "max": [1] },
    { "bufferView": 5, "componentType": 5126, "count": 2, "type": "VEC4" }
  ]
})").arg(Buffer.size()).arg(QString::fromLatin1(Buffer.toBase64())).arg(QString::fromLatin1(TinyPng().toBase64()));

    QFile File(InFolder.filePath(QStringLiteral("Skinned.gltf")));
    File.open(QIODevice::WriteOnly);
    File.write(Json.toUtf8());
    File.close();
}

static void TestAnimationImport(const QDir& InRoot)
{
    std::printf("[animation import]\n");

    /* The defaults: scale 100, z flipped. */
    MeshImportOptions Options;
    const QString Source  = InRoot.filePath(QStringLiteral("Source/Skinned.gltf"));
    const QString Content = InRoot.filePath(QStringLiteral("Content/Anim"));
    const ImportResult Result = ImportMesh(Source, Content, QStringLiteral("Arm"), Options, TextureImportOptions(), InRoot.absolutePath());
    Expect(Result.bOK, "a skinned glTF imports");
    if(!Result.bOK) { std::printf("        %s\n", qPrintable(Result.Message)); return; }
    Expect(Result.Written.size() == 4, "a skeleton, a clip, a mesh and its image are written");
    Expect(QFile::exists(QDir(Content).filePath(QStringLiteral("Skinned_Image0.nmtexture"))),
           "an unnamed embedded image is named for its glTF, so another file's does not overwrite it");

    RenderCore::SkeletonData Skeleton;
    Expect(ReadAsset(QDir(Content).filePath(QStringLiteral("Arm_Skeleton.nmskel")), Skeleton, &RenderCore::SerializeSkeleton),
           "the skeleton reads back");
    Expect(Skeleton.Bones.size() == 2 && Skeleton.Bones[0].Name == "Root" && Skeleton.Bones[1].Name == "Arm"
           && Skeleton.Bones[0].Parent == -1 && Skeleton.Bones[1].Parent == 0, "its bones are the joints, parents first");
    Expect(Near(Skeleton.Bones[1].RefPose.Translation.X, 100.0f), "bone translations are scaled like vertices");

    RenderCore::MeshData Mesh;
    Expect(ReadMesh(QDir(Content).filePath(QStringLiteral("Arm.nmmesh")), Mesh), "the skinned mesh reads back");
    Expect(Mesh.IsSkinned() && Mesh.Skeleton == "Content/Anim/Arm_Skeleton.nmskel", "it is skinned, and names its skeleton by runtime path");
    Expect(Mesh.InverseBind.size() == 2 && Near(Mesh.InverseBind[1].M[0][3], -100.0f), "it carries the converted inverse binds");
    Expect(Mesh.GetVertexCount() == 3 && Near(Mesh.Positions[3], 200.0f) && Near(Mesh.Positions[4], 0.0f),
           "its vertices are in bind space: the node's own transform is ignored");
    Expect(Mesh.Joints[4] == 1 && Near(Mesh.Weights[4], 1.0f) && Mesh.Joints[8] == 0 && Mesh.Joints[9] == 1
           && Near(Mesh.Weights[8] + Mesh.Weights[9], 1.0f), "each vertex keeps its bones and weights");

    RenderCore::AnimClipData Clip;
    const QString ClipPath = QDir(Content).filePath(QStringLiteral("Arm_Bend.nmanim"));
    Expect(ReadAsset(ClipPath, Clip, &RenderCore::SerializeAnimClip), "the clip reads back");
    Expect(Clip.FrameCount == 31 && Near(Clip.GetDuration(), 1.0f), "one second at 30 fps is 31 frames");
    Expect(Clip.Tracks.size() == 1 && Clip.Tracks[0].Bone == "Arm", "only the bone that moves has a track");
    Expect(Clip.Tracks[0].Translations.size() == 1 && Clip.Tracks[0].Rotations.size() == 31, "a constant channel collapses to one key");

    /* The whole chain: at the end, the Arm's vertex (2,0,0) has swung to
     * (1,1,0) -- engine (100, 100, 0). */
    const RenderCore::AnimMatrix ArmModel = RenderCore::AnimMath::Mul(RenderCore::AnimMath::ToMatrix(Skeleton.Bones[0].RefPose),
                                                                      RenderCore::AnimMath::ToMatrix(Clip.SampleTrack(0, 1.0f)));
    const RenderCore::AnimVec3 Moved = RenderCore::AnimMath::TransformPoint(RenderCore::AnimMath::Mul(ArmModel, Mesh.InverseBind[1]),
                                                                            { Mesh.Positions[3], Mesh.Positions[4], Mesh.Positions[5] });
    Expect(std::fabs(Moved.X - 100.0f) < 0.01f && std::fabs(Moved.Y - 100.0f) < 0.01f && std::fabs(Moved.Z) < 0.01f,
           "skinning with the imported bones moves a vertex where glTF says");

    /* A lightmap split (seams duplicate vertices) carries the bones with the copies. */
    RenderCore::MeshData Seamed = Mesh;
    std::string SeamError;
    Expect(GenerateLightmapUVs(Seamed, SeamError) && Seamed.Joints.size() == size_t(Seamed.GetVertexCount()) * 4
           && Seamed.Weights.size() == Seamed.Joints.size(), "a lightmap split keeps every vertex's bones");

    /* What the editor sets on a clip survives a reimport. */
    Clip.Notifies.push_back({ "Footstep", 0.5f });
    Clip.bLoop = false;
    {
        EngineCore::Archive Ar = EngineCore::Archive::ForWriting();
        RenderCore::SerializeAnimClip(Ar, Clip);
        QFile File(ClipPath);
        File.open(QIODevice::WriteOnly);
        File.write(reinterpret_cast<const char*>(Ar.GetBytes().data()), qint64(Ar.GetBytes().size()));
    }
    const ImportResult Again = Reimport(ClipPath, InRoot.absolutePath());
    Expect(Again.bOK, "the clip reimports");
    RenderCore::AnimClipData Back;
    Expect(ReadAsset(ClipPath, Back, &RenderCore::SerializeAnimClip) && Back.Notifies.size() == 1 && !Back.bLoop,
           "keeping its notifies and looping");
    Expect(Reimport(QDir(Content).filePath(QStringLiteral("Arm_Skeleton.nmskel")), InRoot.absolutePath()).bOK, "the skeleton reimports");
    const ImportResult MeshAgain = Reimport(QDir(Content).filePath(QStringLiteral("Arm.nmmesh")), InRoot.absolutePath());
    RenderCore::MeshData MeshBack;
    Expect(MeshAgain.bOK && ReadMesh(QDir(Content).filePath(QStringLiteral("Arm.nmmesh")), MeshBack) && MeshBack.Skeleton == Mesh.Skeleton,
           "the mesh reimports, still naming its skeleton");

    /* Without animation import, the same file is a static mesh. */
    MeshImportOptions Static;
    Static.bImportAnimations = false;
    const ImportResult Plain = ImportMesh(Source, InRoot.filePath(QStringLiteral("Content/Plain")), QStringLiteral("Arm"), Static,
                                          TextureImportOptions(), InRoot.absolutePath());
    RenderCore::MeshData PlainMesh;
    Expect(Plain.bOK && Plain.Written.size() == 2 && ReadMesh(Plain.Written[0], PlainMesh) && !PlainMesh.IsSkinned(),
           "with animations off, it imports static");
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

#if WITH_AUDIO
/* A 16-bit mono WAV of a 220 Hz sine, InSeconds long. */
static void WriteWav(const QString& InPath, const double InSeconds)
{
    const uint32_t Rate = 22050, Frames = uint32_t(InSeconds * Rate), Data = Frames * 2;
    QByteArray Out;
    const auto U32 = [&Out](const uint32_t V) { for(int I = 0; I < 4; ++I) { Out.append(char(V >> (8 * I))); } };
    const auto U16 = [&Out](const uint16_t V) { Out.append(char(V)); Out.append(char(V >> 8)); };
    Out.append("RIFF"); U32(36 + Data); Out.append("WAVE");
    Out.append("fmt "); U32(16); U16(1); U16(1); U32(Rate); U32(Rate * 2); U16(2); U16(16);
    Out.append("data"); U32(Data);
    for(uint32_t Frame = 0; Frame < Frames; ++Frame)
    {
        U16(uint16_t(int16_t(std::lround(0.5 * 32767.0 * std::sin(2.0 * 3.14159265358979 * 220.0 * Frame / Rate)))));
    }
    QFile File(InPath);
    File.open(QIODevice::WriteOnly);
    File.write(Out);
}

static bool ReadSound(const QString& InPath, Audio::SoundData& OutSound)
{
    return ReadAsset(InPath, OutSound, [](EngineCore::Archive& Ar, Audio::SoundData& Data) { return Audio::SerializeSound(Ar, Data); });
}

static void TestSoundImport(const QDir& InRoot)
{
    std::printf("Sound import\n");
    Expect(ClassifySource(QStringLiteral("Engine.WAV")) == EImportKind::Sound, ".wav is a sound, case-insensitively");
    Expect(ClassifySource(QStringLiteral("Music.ogg")) == EImportKind::Sound, ".ogg is a sound");
    Expect(GetImportFileFilter().contains(QStringLiteral("*.flac")), "the file filter offers .flac");
    Expect(GetAssetExtension(EImportKind::Sound) == QStringLiteral(".nmsound"), "sounds become .nmsound");

    const QString Source  = InRoot.filePath(QStringLiteral("Source/Engine.wav"));
    const QString Content = InRoot.filePath(QStringLiteral("Content/Sounds"));
    InRoot.mkpath(QStringLiteral("Content/Sounds"));
    WriteWav(Source, 1.0);

    const ImportResult Result = ImportSound(Source, Content, QStringLiteral("EngineLoop"), InRoot.path());
    Expect(Result.bOK && Result.Written.size() == 1, "a WAV imports");
    const QString Asset = QDir(Content).filePath(QStringLiteral("EngineLoop.nmsound"));

    Audio::SoundData Sound;
    Expect(ReadSound(Asset, Sound), "the .nmsound loads");
    Expect(Sound.IsCooked() && Sound.Cooked.Mode == Audio::ESoundLoadMode::Decompressed, "cooked, Decompressed");
    Expect(Sound.Settings.bSpatial && Sound.Settings.Bus == Audio::EAudioBus::SFX, "short: a spatial SFX");
    Expect(Sound.Import.SourcePath == "Source/Engine.wav", "its source recorded project-relative");

    /* An author's settings survive a reimport from a changed source. */
    Sound.Settings.bLoop  = true;
    Sound.Settings.Volume = 0.4f;
    Expect(Audio::SaveSoundFile(Asset.toStdString(), Sound), "settings changed and saved");
    WriteWav(Source, 1.5);
    const ImportResult Again = Reimport(Asset, InRoot.path());
    Expect(Again.bOK, "it reimports");
    Audio::SoundData After;
    Expect(ReadSound(Asset, After) && After.Settings.bLoop && Near(After.Settings.Volume, 0.4f), "keeping its settings");
    Expect(std::fabs(After.Cooked.GetDuration() - 1.5) < 0.01, "with the new source's length");
}
#endif

int main(int argc, char** argv)
{
    QCoreApplication App(argc, argv);   // image format plugins load through it
    std::printf("Editor import tests\n");

    QTemporaryDir Temp;
    const QDir Root(Temp.path());

    /* NimbleEditorImportTests [--into <project> <folder>] <file.gltf|glb>...:
     * import each into a scratch folder (or <project>/<folder>) and say what
     * came out -- a quick look at a real asset, or test content. */
    QString IntoRoot = Root.absolutePath();
    QString IntoFolder = Root.filePath(QStringLiteral("Look"));
    int FirstFile = 1;
    if(argc > 3 && std::strcmp(argv[1], "--into") == 0)
    {
        IntoRoot   = QFileInfo(QString::fromLocal8Bit(argv[2])).absoluteFilePath();
        IntoFolder = QDir(IntoRoot).filePath(QString::fromLocal8Bit(argv[3]));
        FirstFile  = 4;
    }
    for(int Arg = FirstFile; Arg < argc; ++Arg)
    {
        const QString Path = QString::fromLocal8Bit(argv[Arg]);
        const ImportResult Each = ImportMesh(Path, IntoFolder, QFileInfo(Path).completeBaseName(),
                                             MeshImportOptions(), TextureImportOptions(), IntoRoot);
        std::printf("%s: %s\n", qPrintable(Path), qPrintable(Each.Message));
        for(const QString& Warning : Each.Warnings) { std::printf("  warning: %s\n", qPrintable(Warning)); }
        for(const QString& Written : Each.Written)  { std::printf("  wrote %s\n", qPrintable(QFileInfo(Written).fileName())); }

        /* A skinned mesh's extent at rest and posed: a mesh that binds wrong shows it here. */
        RenderCore::SkeletonData Skeleton;
        RenderCore::MeshData     Mesh;
        std::vector<RenderCore::AnimClipData> Clips;
        for(const QString& Written : Each.Written)
        {
            if(Written.endsWith(QStringLiteral(".nmskel"))) { ReadAsset(Written, Skeleton, &RenderCore::SerializeSkeleton); }
            if(Written.endsWith(QStringLiteral(".nmmesh"))) { ReadMesh(Written, Mesh); }
            if(Written.endsWith(QStringLiteral(".nmanim"))) { Clips.emplace_back(); ReadAsset(Written, Clips.back(), &RenderCore::SerializeAnimClip); }
        }
        if(!Mesh.IsSkinned() || Skeleton.Bones.empty()) { continue; }
        const auto Extent = [&](const std::vector<RenderCore::AnimMatrix>* InModel, const char* InWhat)
        {
            float Min[3] = { 1e30f, 1e30f, 1e30f }, Max[3] = { -1e30f, -1e30f, -1e30f };
            for(uint32_t Vertex = 0; Vertex < Mesh.GetVertexCount(); ++Vertex)
            {
                const RenderCore::AnimVec3 P{ Mesh.Positions[Vertex * 3], Mesh.Positions[Vertex * 3 + 1], Mesh.Positions[Vertex * 3 + 2] };
                RenderCore::AnimVec3 Out{ 0, 0, 0 };
                for(int Influence = 0; Influence < 4; ++Influence)
                {
                    const float Weight = Mesh.Weights[Vertex * 4 + Influence];
                    const uint16_t Bone = Mesh.Joints[Vertex * 4 + Influence];
                    const RenderCore::AnimVec3 Moved = InModel
                        ? RenderCore::AnimMath::TransformPoint(RenderCore::AnimMath::Mul((*InModel)[Bone], Mesh.InverseBind[Bone]), P) : P;
                    Out = RenderCore::AnimMath::Add(Out, RenderCore::AnimMath::Scale(Moved, Weight));
                }
                const float At[3] = { Out.X, Out.Y, Out.Z };
                for(int Axis = 0; Axis < 3; ++Axis) { Min[Axis] = std::min(Min[Axis], At[Axis]); Max[Axis] = std::max(Max[Axis], At[Axis]); }
            }
            std::printf("  %-22s x %8.1f..%8.1f  y %8.1f..%8.1f  z %8.1f..%8.1f\n", InWhat, Min[0], Max[0], Min[1], Max[1], Min[2], Max[2]);
        };
        Extent(nullptr, "bind space");
        std::vector<RenderCore::AnimMatrix> Model;
        Skeleton.ComputeRefPoseModel(Model);
        Extent(&Model, "reference pose");
        for(const RenderCore::AnimClipData& Clip : Clips)
        {
            std::vector<RenderCore::AnimTransform> Local(Skeleton.Bones.size());
            for(size_t Bone = 0; Bone < Skeleton.Bones.size(); ++Bone) { Local[Bone] = Skeleton.Bones[Bone].RefPose; }
            for(size_t Track = 0; Track < Clip.Tracks.size(); ++Track)
            {
                const int32_t Bone = Skeleton.FindBone(Clip.Tracks[Track].Bone);
                if(Bone >= 0) { Local[size_t(Bone)] = Clip.SampleTrack(Track, 0.0f); }
            }
            for(size_t Bone = 0; Bone < Skeleton.Bones.size(); ++Bone)
            {
                const RenderCore::AnimMatrix L = RenderCore::AnimMath::ToMatrix(Local[Bone]);
                Model[Bone] = Skeleton.Bones[Bone].Parent >= 0 ? RenderCore::AnimMath::Mul(Model[size_t(Skeleton.Bones[Bone].Parent)], L) : L;
            }
            Extent(&Model, "clip frame 0");
        }
    }
    if(argc > 1)
    {
        return 0;
    }
    Root.mkpath(QStringLiteral("Source"));
    WriteScene(QDir(Root.filePath(QStringLiteral("Source"))));
    WriteSkinnedScene(QDir(Root.filePath(QStringLiteral("Source"))));

    TestClassify();
    TestMeshImport(Root);
    TestTextureImport(Root);
    TestAnimationImport(Root);
#if WITH_AUDIO
    TestSoundImport(Root);
#endif

    std::printf("%d checks, %d failures\n", GChecks, GFailures);
    return GFailures == 0 ? 0 : 1;
}
