#if WITH_EDITOR

#include "Private/Editor/Content/GltfImporter.h"

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>

#include <QDir>
#include <QFile>
#include <QFileInfo>

/* The one translation unit that compiles cgltf. Its stdio use is replaced
 * below (ReadFile), so MSVC's fopen deprecation does not apply. */
#define CGLTF_IMPLEMENTATION
#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif
#include "cgltf.h"
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

namespace EditorContent
{
    namespace
    {
        /* cgltf's fopen reads paths as ANSI on Windows, breaking non-ASCII
         * folders; QFile reads the .gltf and its .bin files instead. */
        cgltf_result ReadFile(const cgltf_memory_options*, const cgltf_file_options*, const char* InPath,
                              cgltf_size* OutSize, void** OutData)
        {
            QFile File(QString::fromUtf8(InPath));
            if(!File.open(QIODevice::ReadOnly))
            {
                return cgltf_result_file_not_found;
            }
            const QByteArray Bytes = File.readAll();
            void* Data = std::malloc(size_t(Bytes.size()) > 0 ? size_t(Bytes.size()) : 1);
            if(!Data)
            {
                return cgltf_result_out_of_memory;
            }
            std::memcpy(Data, Bytes.constData(), size_t(Bytes.size()));
            *OutSize = cgltf_size(Bytes.size());
            *OutData = Data;
            return cgltf_result_success;
        }

        void ReleaseFile(const cgltf_memory_options*, const cgltf_file_options*, void* InData)
        {
            std::free(InData);
        }

        const char* DescribeResult(const cgltf_result InResult)
        {
            switch(InResult)
            {
                case cgltf_result_success:         return "success";
                case cgltf_result_data_too_short:  return "the file is truncated";
                case cgltf_result_unknown_format:  return "not a glTF or GLB file";
                case cgltf_result_invalid_json:    return "the JSON is malformed";
                case cgltf_result_invalid_gltf:    return "the glTF is invalid";
                case cgltf_result_invalid_options: return "invalid options";
                case cgltf_result_file_not_found:  return "a file it needs was not found";
                case cgltf_result_io_error:        return "a file could not be read";
                case cgltf_result_out_of_memory:   return "out of memory";
                case cgltf_result_legacy_gltf:     return "glTF 1.0 is not supported";
                default:                           return "unknown error";
            }
        }

        /* A parsed file with its buffers loaded, freed on scope exit. */
        class GltfFile
        {
        public:
            ~GltfFile() { if(Data) { cgltf_free(Data); } }

            bool Load(const QString& InPath, QString& OutError)
            {
                cgltf_options Options{};
                Options.file.read    = &ReadFile;
                Options.file.release = &ReleaseFile;

                const QByteArray Path = QFileInfo(InPath).absoluteFilePath().toUtf8();
                cgltf_result Result = cgltf_parse_file(&Options, Path.constData(), &Data);
                if(Result == cgltf_result_success)
                {
                    Result = cgltf_load_buffers(&Options, Data, Path.constData());
                }
                if(Result == cgltf_result_success)
                {
                    Result = cgltf_validate(Data);
                }
                if(Result != cgltf_result_success)
                {
                    OutError = QStringLiteral("%1: %2").arg(QFileInfo(InPath).fileName(), QString::fromLatin1(DescribeResult(Result)));
                    return false;
                }
                return true;
            }

            cgltf_data* Data = nullptr;
        };

        QString NameOf(const char* InName, const QString& InFallback)
        {
            return (InName && InName[0] != '\0') ? QString::fromUtf8(InName) : InFallback;
        }

        /* Column-major, as glTF and cgltf store it: M[col * 4 + row]. */
        struct Transform
        {
            float M[16];
            float Normal[9];     // inverse transpose of the upper 3x3, row-major
            bool  bMirrors = false;

            void Prepare()
            {
                const float A = M[0], B = M[4], C = M[8];
                const float D = M[1], E = M[5], F = M[9];
                const float G = M[2], H = M[6], I = M[10];
                const float Det = A * (E * I - F * H) - B * (D * I - F * G) + C * (D * H - E * G);
                bMirrors = Det < 0.0f;
                const float Inv = std::fabs(Det) > 1e-20f ? 1.0f / Det : 0.0f;
                /* The inverse transpose is the cofactor matrix over the determinant. */
                Normal[0] = (E * I - F * H) * Inv; Normal[1] = (F * G - D * I) * Inv; Normal[2] = (D * H - E * G) * Inv;
                Normal[3] = (C * H - B * I) * Inv; Normal[4] = (A * I - C * G) * Inv; Normal[5] = (B * G - A * H) * Inv;
                Normal[6] = (B * F - C * E) * Inv; Normal[7] = (C * D - A * F) * Inv; Normal[8] = (A * E - B * D) * Inv;
            }

            void Point(const float* InP, float* OutP) const
            {
                for(int Row = 0; Row < 3; ++Row)
                {
                    OutP[Row] = M[Row] * InP[0] + M[4 + Row] * InP[1] + M[8 + Row] * InP[2] + M[12 + Row];
                }
            }

            void Direction(const float* InN, float* OutN) const
            {
                for(int Row = 0; Row < 3; ++Row)
                {
                    OutN[Row] = Normal[Row * 3 + 0] * InN[0] + Normal[Row * 3 + 1] * InN[1] + Normal[Row * 3 + 2] * InN[2];
                }
            }
        };

        void Normalize(float* InOutV)
        {
            const float Length = std::sqrt(InOutV[0] * InOutV[0] + InOutV[1] * InOutV[1] + InOutV[2] * InOutV[2]);
            if(Length > 1e-20f)
            {
                InOutV[0] /= Length; InOutV[1] /= Length; InOutV[2] /= Length;
            }
        }

        const cgltf_accessor* FindAttribute(const cgltf_primitive& InPrimitive, const cgltf_attribute_type InType,
                                            const int InIndex)
        {
            for(cgltf_size At = 0; At < InPrimitive.attributes_count; ++At)
            {
                const cgltf_attribute& Attribute = InPrimitive.attributes[At];
                if(Attribute.type == InType && Attribute.index == InIndex) { return Attribute.data; }
            }
            return nullptr;
        }

        /* Area-weighted smooth normals for vertices / triangles from the
         * given offsets on. */
        void GenerateNormals(RenderCore::MeshData& InOutMesh, const uint32_t InFirstVertex, const size_t InFirstIndex)
        {
            for(size_t At = InFirstIndex; At + 2 < InOutMesh.Indices.size(); At += 3)
            {
                const uint32_t Corner[3] = { InOutMesh.Indices[At], InOutMesh.Indices[At + 1], InOutMesh.Indices[At + 2] };
                const float* P0 = &InOutMesh.Positions[size_t(Corner[0]) * 3];
                const float* P1 = &InOutMesh.Positions[size_t(Corner[1]) * 3];
                const float* P2 = &InOutMesh.Positions[size_t(Corner[2]) * 3];
                const float U[3] = { P1[0] - P0[0], P1[1] - P0[1], P1[2] - P0[2] };
                const float V[3] = { P2[0] - P0[0], P2[1] - P0[1], P2[2] - P0[2] };
                const float Face[3] = { U[1] * V[2] - U[2] * V[1], U[2] * V[0] - U[0] * V[2], U[0] * V[1] - U[1] * V[0] };
                for(const uint32_t Vertex : Corner)
                {
                    for(int Axis = 0; Axis < 3; ++Axis) { InOutMesh.Normals[size_t(Vertex) * 3 + size_t(Axis)] += Face[Axis]; }
                }
            }
            for(size_t Vertex = InFirstVertex; Vertex < InOutMesh.GetVertexCount(); ++Vertex)
            {
                Normalize(&InOutMesh.Normals[Vertex * 3]);
            }
        }

        /* One primitive onto InOutMesh as a new section. False (with a
         * warning) if it is skipped. */
        bool AppendPrimitive(RenderCore::MeshData& InOutMesh, const cgltf_primitive& InPrimitive, const Transform& InTransform,
                             const MeshImportOptions& InOptions, const QString& InSectionName, QStringList& OutWarnings)
        {
            if(InPrimitive.type != cgltf_primitive_type_triangles)
            {
                OutWarnings << QStringLiteral("%1: skipped -- only triangle primitives are imported").arg(InSectionName);
                return false;
            }

            const cgltf_accessor* Positions = FindAttribute(InPrimitive, cgltf_attribute_type_position, 0);
            const cgltf_accessor* Normals   = FindAttribute(InPrimitive, cgltf_attribute_type_normal, 0);
            const cgltf_accessor* UVs       = FindAttribute(InPrimitive, cgltf_attribute_type_texcoord, 0);
            const cgltf_accessor* Colors    = FindAttribute(InPrimitive, cgltf_attribute_type_color, 0);
            if(!Positions || Positions->count == 0)
            {
                OutWarnings << QStringLiteral("%1: skipped -- it has no positions").arg(InSectionName);
                return false;
            }

            const cgltf_size VertexCount = Positions->count;
            const cgltf_size IndexCount  = InPrimitive.indices ? InPrimitive.indices->count : VertexCount;
            if(IndexCount < 3 || IndexCount % 3 != 0)
            {
                OutWarnings << QStringLiteral("%1: skipped -- its indices are not whole triangles").arg(InSectionName);
                return false;
            }
            if(uint64_t(InOutMesh.GetVertexCount()) + VertexCount > UINT32_MAX)
            {
                OutWarnings << QStringLiteral("%1: skipped -- the mesh would have too many vertices").arg(InSectionName);
                return false;
            }

            /* The indices first, so a bad one skips the primitive before any
             * of its vertices are added. */
            const uint32_t BaseVertex = InOutMesh.GetVertexCount();
            std::vector<uint32_t> Local(IndexCount);
            for(cgltf_size At = 0; At < IndexCount; ++At)
            {
                const cgltf_size Index = InPrimitive.indices ? cgltf_accessor_read_index(InPrimitive.indices, At) : At;
                if(Index >= VertexCount)
                {
                    OutWarnings << QStringLiteral("%1: skipped -- an index is past its last vertex").arg(InSectionName);
                    return false;
                }
                Local[At] = uint32_t(Index);
            }

            /* glTF -> engine: x stays, y and z flip (a half turn about X). */
            const float Flip = InOptions.bConvertCoordinates ? -1.0f : 1.0f;

            /* Vertex colours (COLOR_0) are all or none in a mesh: the first
             * primitive with them backfills white for the vertices before it,
             * and a primitive without them after that is white. */
            if(Colors && InOutMesh.Colors.empty())
            {
                InOutMesh.Colors.assign(size_t(BaseVertex) * 4, 1.0f);
            }

            for(cgltf_size Vertex = 0; Vertex < VertexCount; ++Vertex)
            {
                float Source[3] = {0.0f, 0.0f, 0.0f};
                float Placed[3];
                cgltf_accessor_read_float(Positions, Vertex, Source, 3);
                InTransform.Point(Source, Placed);
                InOutMesh.Positions.push_back(Placed[0] * InOptions.Scale);
                InOutMesh.Positions.push_back(Placed[1] * InOptions.Scale * Flip);
                InOutMesh.Positions.push_back(Placed[2] * InOptions.Scale * Flip);

                float Normal[3] = {0.0f, 0.0f, 0.0f};
                if(Normals)
                {
                    float Read[3] = {0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_float(Normals, Vertex, Read, 3);
                    InTransform.Direction(Read, Normal);
                    Normalize(Normal);
                }
                InOutMesh.Normals.push_back(Normal[0]);
                InOutMesh.Normals.push_back(Normal[1] * Flip);
                InOutMesh.Normals.push_back(Normal[2] * Flip);

                float UV[2] = {0.0f, 0.0f};
                if(UVs) { cgltf_accessor_read_float(UVs, Vertex, UV, 2); }
                InOutMesh.UV0.push_back(UV[0]);
                InOutMesh.UV0.push_back(UV[1]);

                if(!InOutMesh.Colors.empty())
                {
                    /* Float or normalised integers alike; a vec3 colour keeps alpha 1. */
                    float Color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    if(Colors) { cgltf_accessor_read_float(Colors, Vertex, Color, 4); }
                    InOutMesh.Colors.insert(InOutMesh.Colors.end(), Color, Color + 4);
                }
            }

            /* A mirroring transform flips winding; swap two corners to undo it. */
            const size_t FirstIndex = InOutMesh.Indices.size();
            for(cgltf_size At = 0; At < IndexCount; At += 3)
            {
                InOutMesh.Indices.push_back(BaseVertex + Local[At]);
                InOutMesh.Indices.push_back(BaseVertex + Local[InTransform.bMirrors ? At + 2 : At + 1]);
                InOutMesh.Indices.push_back(BaseVertex + Local[InTransform.bMirrors ? At + 1 : At + 2]);
            }

            if(!Normals)
            {
                if(InOptions.bGenerateNormals)
                {
                    GenerateNormals(InOutMesh, BaseVertex, FirstIndex);
                }
                else
                {
                    OutWarnings << QStringLiteral("%1: has no normals; it will draw dark").arg(InSectionName);
                }
            }

            RenderCore::MeshSection Section;
            Section.Name       = InSectionName.toStdString();
            Section.Material   = (InPrimitive.material && InPrimitive.material->name) ? InPrimitive.material->name : "";
            Section.FirstIndex = uint32_t(FirstIndex);
            Section.IndexCount = uint32_t(IndexCount);
            InOutMesh.Sections.push_back(Section);
            return true;
        }

        void AppendMesh(RenderCore::MeshData& InOutMesh, const cgltf_mesh& InMesh, const QString& InName,
                        const Transform& InTransform, const MeshImportOptions& InOptions, QStringList& OutWarnings)
        {
            for(cgltf_size At = 0; At < InMesh.primitives_count; ++At)
            {
                const QString Section = InMesh.primitives_count > 1 ? QStringLiteral("%1.%2").arg(InName).arg(At) : InName;
                AppendPrimitive(InOutMesh, InMesh.primitives[At], InTransform, InOptions, Section, OutWarnings);
            }
        }

        void CollectImages(const cgltf_data& InData, const QString& InGltfPath, QList<ReferencedImage>& OutImages)
        {
            const QDir Folder = QFileInfo(InGltfPath).absoluteDir();
            for(cgltf_size At = 0; At < InData.images_count; ++At)
            {
                const cgltf_image& Image = InData.images[At];
                ReferencedImage Found;
                Found.Index = int(At);

                if(Image.buffer_view && Image.buffer_view->buffer && Image.buffer_view->buffer->data)
                {
                    const auto* Bytes = static_cast<const char*>(Image.buffer_view->buffer->data) + Image.buffer_view->offset;
                    Found.Embedded = QByteArray(Bytes, qsizetype(Image.buffer_view->size));
                }
                else if(Image.uri && std::strncmp(Image.uri, "data:", 5) == 0)
                {
                    const char* Comma = std::strchr(Image.uri, ',');
                    if(Comma) { Found.Embedded = QByteArray::fromBase64(QByteArray(Comma + 1)); }
                }
                else if(Image.uri)
                {
                    QByteArray Uri(Image.uri);
                    cgltf_decode_uri(Uri.data());   // %20 and the like
                    Found.SourcePath = QDir::cleanPath(Folder.absoluteFilePath(QString::fromUtf8(Uri.constData())));
                }

                const QString FromFile = Found.SourcePath.isEmpty() ? QString() : QFileInfo(Found.SourcePath).completeBaseName();
                Found.Name = NameOf(Image.name, FromFile.isEmpty() ? QStringLiteral("Image%1").arg(At) : FromFile);
                OutImages << Found;
            }
        }
    }

    bool LoadGltf(const QString& InPath, const MeshImportOptions& InOptions, QList<GltfMesh>& OutMeshes,
                  QList<ReferencedImage>& OutImages, QStringList& OutWarnings, QString& OutError)
    {
        GltfFile File;
        if(!File.Load(InPath, OutError))
        {
            return false;
        }
        const cgltf_data& Data = *File.Data;

        OutMeshes.clear();
        if(InOptions.bCombineMeshes)
        {
            OutMeshes << GltfMesh{ QStringLiteral("Mesh"), RenderCore::MeshData() };
        }

        std::set<QString> UsedNames;
        const auto MeshFor = [&](const QString& InName) -> RenderCore::MeshData&
        {
            if(InOptions.bCombineMeshes) { return OutMeshes.front().Data; }

            QString Unique = InName;
            for(int Suffix = 2; UsedNames.count(Unique) != 0; ++Suffix) { Unique = QStringLiteral("%1_%2").arg(InName).arg(Suffix); }
            UsedNames.insert(Unique);
            OutMeshes << GltfMesh{ Unique, RenderCore::MeshData() };
            return OutMeshes.back().Data;
        };

        /* The scene's nodes, depth first, each mesh with its world transform. */
        int MeshesPlaced = 0;
        std::function<void(const cgltf_node&)> Visit = [&](const cgltf_node& InNode)
        {
            if(InNode.mesh)
            {
                Transform Placement;
                cgltf_node_transform_world(&InNode, Placement.M);
                Placement.Prepare();
                const QString Name = NameOf(InNode.mesh->name, NameOf(InNode.name, QStringLiteral("Mesh%1").arg(MeshesPlaced)));
                AppendMesh(MeshFor(Name), *InNode.mesh, Name, Placement, InOptions, OutWarnings);
                ++MeshesPlaced;
            }
            for(cgltf_size At = 0; At < InNode.children_count; ++At) { Visit(*InNode.children[At]); }
        };

        const cgltf_scene* Scene = Data.scene ? Data.scene : (Data.scenes_count > 0 ? &Data.scenes[0] : nullptr);
        if(Scene)
        {
            for(cgltf_size At = 0; At < Scene->nodes_count; ++At) { Visit(*Scene->nodes[At]); }
        }
        else
        {
            for(cgltf_size At = 0; At < Data.nodes_count; ++At)
            {
                if(!Data.nodes[At].parent) { Visit(Data.nodes[At]); }
            }
        }

        /* No node places a mesh: take the meshes as they are. */
        if(MeshesPlaced == 0)
        {
            Transform Identity{};
            Identity.M[0] = Identity.M[5] = Identity.M[10] = Identity.M[15] = 1.0f;
            Identity.Prepare();
            for(cgltf_size At = 0; At < Data.meshes_count; ++At)
            {
                const QString Name = NameOf(Data.meshes[At].name, QStringLiteral("Mesh%1").arg(At));
                AppendMesh(MeshFor(Name), Data.meshes[At], Name, Identity, InOptions, OutWarnings);
            }
        }

        /* Drop what came out empty; the file must have given something. */
        for(qsizetype At = OutMeshes.size() - 1; At >= 0; --At)
        {
            if(OutMeshes[At].Data.Indices.empty()) { OutMeshes.removeAt(At); }
        }
        if(OutMeshes.isEmpty())
        {
            OutError = QStringLiteral("%1: no triangle meshes to import").arg(QFileInfo(InPath).fileName());
            return false;
        }
        for(GltfMesh& Each : OutMeshes) { RenderCore::ComputeMeshBounds(Each.Data); }

        CollectImages(Data, InPath, OutImages);
        return true;
    }

    bool LoadGltfImages(const QString& InPath, QList<ReferencedImage>& OutImages, QString& OutError)
    {
        GltfFile File;
        if(!File.Load(InPath, OutError))
        {
            return false;
        }
        CollectImages(*File.Data, InPath, OutImages);
        return true;
    }
}

#endif // WITH_EDITOR
