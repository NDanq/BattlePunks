#if WITH_EDITOR

#include "Private/Editor/Content/GltfImporter.h"
#include "Private/Editor/Content/LightmapUVs.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <set>
#include <unordered_map>

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

        // === Skeletons and animation (ANIMATION.md) ================================

        /*
         * glTF -> engine for transforms. Vertices convert by C = diag(s, s, f s)
         * (f = -1 when converting coordinates); a bone's transform converts by
         * conjugation, C M C^-1, so that skinning with converted bones and
         * converted inverse binds places a converted vertex exactly where C
         * would have put the glTF one. The scale cancels from rotations; the
         * mirror turns a quaternion (x, y, z, w) into (-x, -y, z, w).
         */
        struct Conversion
        {
            float Scale = 1.0f;
            float Flip  = 1.0f;

            explicit Conversion(const MeshImportOptions& InOptions)
                : Scale(InOptions.Scale), Flip(InOptions.bConvertCoordinates ? -1.0f : 1.0f) {}

            RenderCore::AnimTransform Local(const RenderCore::AnimTransform& InT) const
            {
                RenderCore::AnimTransform Out = InT;
                Out.Translation = { InT.Translation.X * Scale, InT.Translation.Y * Scale, InT.Translation.Z * Scale * Flip };
                if(Flip < 0.0f)
                {
                    Out.Rotation = { -InT.Rotation.X, -InT.Rotation.Y, InT.Rotation.Z, InT.Rotation.W };
                }
                return Out;
            }

            RenderCore::AnimMatrix Matrix(const RenderCore::AnimMatrix& InM) const
            {
                const float F[3] = { 1.0f, 1.0f, Flip };
                RenderCore::AnimMatrix Out;
                for(int Row = 0; Row < 3; ++Row)
                {
                    for(int Col = 0; Col < 3; ++Col) { Out.M[Row][Col] = InM.M[Row][Col] * F[Row] * F[Col]; }
                    Out.M[Row][3] = InM.M[Row][3] * F[Row] * Scale;
                }
                return Out;
            }
        };

        /* Column-major glTF 4 x 4 -> AnimMatrix (its last row is dropped). */
        RenderCore::AnimMatrix FromGltfMatrix(const float* InM)
        {
            RenderCore::AnimMatrix Out;
            for(int Row = 0; Row < 3; ++Row)
            {
                for(int Col = 0; Col < 4; ++Col) { Out.M[Row][Col] = InM[Col * 4 + Row]; }
            }
            return Out;
        }

        /* A node's own transform, glTF space. */
        RenderCore::AnimTransform NodeLocal(const cgltf_node& InNode)
        {
            if(InNode.has_matrix)
            {
                return RenderCore::AnimMath::ToTransform(FromGltfMatrix(InNode.matrix));
            }
            RenderCore::AnimTransform Out;
            if(InNode.has_translation) { Out.Translation = { InNode.translation[0], InNode.translation[1], InNode.translation[2] }; }
            if(InNode.has_rotation)
            {
                Out.Rotation = RenderCore::AnimMath::Normalize({ InNode.rotation[0], InNode.rotation[1], InNode.rotation[2], InNode.rotation[3] });
            }
            if(InNode.has_scale)       { Out.Scale = { InNode.scale[0], InNode.scale[1], InNode.scale[2] }; }
            return Out;
        }

        /* The file's skeleton: every skin's joints and their ancestors, parents
         * first. Bones by node, and each bone's inverse bind (engine space). */
        struct SkeletonBuild
        {
            RenderCore::SkeletonData                       Skeleton;
            std::unordered_map<const cgltf_node*, int32_t> BoneOf;
            std::vector<const cgltf_node*>                 Nodes;         // per bone
            std::vector<RenderCore::AnimMatrix>            InverseBind;

            int32_t Find(const cgltf_node* InNode) const
            {
                const auto Found = BoneOf.find(InNode);
                return Found != BoneOf.end() ? Found->second : -1;
            }

            /* InNode's bone, else its nearest ancestor's; the first bone if none. */
            int32_t FindNearest(const cgltf_node* InNode) const
            {
                for(const cgltf_node* At = InNode; At; At = At->parent)
                {
                    const int32_t Bone = Find(At);
                    if(Bone >= 0) { return Bone; }
                }
                return 0;
            }
        };

        bool BuildSkeleton(const cgltf_data& InData, const Conversion& InConvert, SkeletonBuild& OutBuild, QStringList& OutWarnings)
        {
            std::set<const cgltf_node*> Wanted;
            for(cgltf_size SkinAt = 0; SkinAt < InData.skins_count; ++SkinAt)
            {
                const cgltf_skin& Skin = InData.skins[SkinAt];
                for(cgltf_size JointAt = 0; JointAt < Skin.joints_count; ++JointAt)
                {
                    for(const cgltf_node* At = Skin.joints[JointAt]; At; At = At->parent) { Wanted.insert(At); }
                }
            }
            if(Wanted.empty())
            {
                return false;
            }

            std::set<std::string> Names;
            bool bTooMany = false;
            std::function<void(const cgltf_node&, int32_t)> Visit = [&](const cgltf_node& InNode, const int32_t InParent)
            {
                int32_t Here = InParent;
                if(Wanted.count(&InNode) != 0)
                {
                    if(OutBuild.Skeleton.Bones.size() >= RenderCore::SKELETON_MAX_BONES)
                    {
                        bTooMany = true;
                    }
                    else
                    {
                        const std::string Base = NameOf(InNode.name, QStringLiteral("Bone%1").arg(OutBuild.Skeleton.Bones.size())).toStdString();
                        std::string Name = Base;
                        for(int Suffix = 2; Names.count(Name) != 0; ++Suffix) { Name = Base + "_" + std::to_string(Suffix); }
                        Names.insert(Name);

                        RenderCore::SkeletonBone Bone;
                        Bone.Name    = Name;
                        Bone.Parent  = InParent;
                        Bone.RefPose = InConvert.Local(NodeLocal(InNode));
                        Here = int32_t(OutBuild.Skeleton.Bones.size());
                        OutBuild.BoneOf[&InNode] = Here;
                        OutBuild.Nodes.push_back(&InNode);
                        OutBuild.Skeleton.Bones.push_back(Bone);
                    }
                }
                for(cgltf_size At = 0; At < InNode.children_count; ++At) { Visit(*InNode.children[At], Here); }
            };
            for(cgltf_size At = 0; At < InData.nodes_count; ++At)
            {
                if(!InData.nodes[At].parent) { Visit(InData.nodes[At], -1); }
            }
            if(bTooMany)
            {
                OutWarnings << QStringLiteral("the skeleton has more than %1 bones; the rest are left out").arg(RenderCore::SKELETON_MAX_BONES);
            }

            /* A bone no skin binds -- an ancestor of the joints -- binds at its
             * reference pose; a joint at its skin's inverse bind matrix (the
             * first skin's, if several name it). */
            std::vector<RenderCore::AnimMatrix> RefModel;
            OutBuild.Skeleton.ComputeRefPoseModel(RefModel);
            OutBuild.InverseBind.resize(RefModel.size());
            for(size_t Bone = 0; Bone < RefModel.size(); ++Bone)
            {
                OutBuild.InverseBind[Bone] = RenderCore::AnimMath::Inverse(RefModel[Bone]);
            }
            std::vector<bool> FromSkin(RefModel.size(), false);
            for(cgltf_size SkinAt = 0; SkinAt < InData.skins_count; ++SkinAt)
            {
                const cgltf_skin& Skin = InData.skins[SkinAt];
                for(cgltf_size JointAt = 0; JointAt < Skin.joints_count; ++JointAt)
                {
                    const int32_t Bone = OutBuild.Find(Skin.joints[JointAt]);
                    if(Bone < 0 || FromSkin[size_t(Bone)]) { continue; }
                    float Matrix[16] = { 1, 0, 0, 0,  0, 1, 0, 0,  0, 0, 1, 0,  0, 0, 0, 1 };
                    if(Skin.inverse_bind_matrices) { cgltf_accessor_read_float(Skin.inverse_bind_matrices, JointAt, Matrix, 16); }
                    OutBuild.InverseBind[size_t(Bone)] = InConvert.Matrix(FromGltfMatrix(Matrix));
                    FromSkin[size_t(Bone)] = true;
                }
                if(SkinAt > 0)
                {
                    OutWarnings << QStringLiteral("several skins: they share one skeleton, binding as the first that names each bone");
                }
            }
            return true;
        }

        /* How a primitive's vertices bind: to Skin's joints, or all rigidly to RigidBone. */
        struct PrimitiveSkin
        {
            const SkeletonBuild* Build     = nullptr;
            const cgltf_skin*    Skin      = nullptr;
            int32_t              RigidBone = 0;
        };

        /* Vertex InVertex's four strongest influences, as skeleton bones, normalised. */
        void ReadInfluences(const cgltf_primitive& InPrimitive, const PrimitiveSkin& InSkin, const cgltf_size InVertex,
                            uint16_t OutJoints[4], float OutWeights[4])
        {
            std::pair<float, int32_t> Influences[8];
            int Count = 0;
            if(InSkin.Skin)
            {
                for(int Set = 0; Set < 2; ++Set)
                {
                    const cgltf_accessor* Joints  = FindAttribute(InPrimitive, cgltf_attribute_type_joints, Set);
                    const cgltf_accessor* Weights = FindAttribute(InPrimitive, cgltf_attribute_type_weights, Set);
                    if(!Joints || !Weights || InVertex >= Joints->count || InVertex >= Weights->count) { continue; }
                    cgltf_uint JointIndex[4] = {0, 0, 0, 0};
                    float      Weight[4]     = {0.0f, 0.0f, 0.0f, 0.0f};
                    cgltf_accessor_read_uint(Joints, InVertex, JointIndex, 4);
                    cgltf_accessor_read_float(Weights, InVertex, Weight, 4);
                    for(int Each = 0; Each < 4; ++Each)
                    {
                        if(Weight[Each] <= 0.0f || JointIndex[Each] >= InSkin.Skin->joints_count) { continue; }
                        const int32_t Bone = InSkin.Build->Find(InSkin.Skin->joints[JointIndex[Each]]);
                        if(Bone >= 0) { Influences[Count++] = { Weight[Each], Bone }; }
                    }
                }
            }
            std::sort(Influences, Influences + Count, [](const auto& InA, const auto& InB) { return InA.first > InB.first; });
            Count = std::min(Count, 4);

            float Sum = 0.0f;
            for(int Each = 0; Each < Count; ++Each) { Sum += Influences[Each].first; }
            for(int Each = 0; Each < 4; ++Each)
            {
                const bool bUsed = Each < Count && Sum > 0.0f;
                OutJoints[Each]  = bUsed ? uint16_t(Influences[Each].second) : 0;
                OutWeights[Each] = bUsed ? Influences[Each].first / Sum : 0.0f;
            }
            if(Count == 0 || Sum <= 0.0f)
            {
                OutJoints[0]  = uint16_t(InSkin.RigidBone);
                OutWeights[0] = 1.0f;
            }
        }

        /* One animation channel, read once: its key times and values. */
        struct ChannelKeys
        {
            const cgltf_animation_channel* Channel = nullptr;
            int                Components = 3;     // 4 for rotation
            bool               bCubic     = false;
            bool               bStep      = false;
            std::vector<float> Times;
            std::vector<float> Values;              // Components per key; x3 (in, value, out) if cubic

            bool Read(const cgltf_animation_channel& InChannel)
            {
                Channel = &InChannel;
                const cgltf_animation_sampler* Sampler = InChannel.sampler;
                if(!Sampler || !Sampler->input || !Sampler->output || Sampler->input->count == 0) { return false; }
                Components = InChannel.target_path == cgltf_animation_path_type_rotation ? 4 : 3;
                bCubic = Sampler->interpolation == cgltf_interpolation_type_cubic_spline;
                bStep  = Sampler->interpolation == cgltf_interpolation_type_step;
                const cgltf_size Keys = Sampler->input->count;
                const cgltf_size PerKey = bCubic ? 3 : 1;
                if(Sampler->output->count < Keys * PerKey) { return false; }
                Times.resize(Keys);
                for(cgltf_size At = 0; At < Keys; ++At) { cgltf_accessor_read_float(Sampler->input, At, &Times[At], 1); }
                Values.resize(Keys * PerKey * size_t(Components));
                for(cgltf_size At = 0; At < Keys * PerKey; ++At)
                {
                    cgltf_accessor_read_float(Sampler->output, At, &Values[At * size_t(Components)], cgltf_size(Components));
                }
                return true;
            }

            const float* Value(const size_t InKey, const int InPart = 1) const
            {
                const size_t Element = bCubic ? InKey * 3 + size_t(InPart) : InKey;
                return &Values[Element * size_t(Components)];
            }

            void Sample(const float InTime, float* OutValue) const
            {
                const size_t Last = Times.size() - 1;
                if(InTime <= Times.front() || Last == 0) { std::memcpy(OutValue, Value(0), sizeof(float) * size_t(Components)); return; }
                if(InTime >= Times.back())               { std::memcpy(OutValue, Value(Last), sizeof(float) * size_t(Components)); return; }

                const size_t B = size_t(std::upper_bound(Times.begin(), Times.end(), InTime) - Times.begin());
                const size_t A = B - 1;
                const float  Span = Times[B] - Times[A];
                const float  T = Span > 0.0f ? (InTime - Times[A]) / Span : 0.0f;
                if(bStep)
                {
                    std::memcpy(OutValue, Value(A), sizeof(float) * size_t(Components));
                }
                else if(bCubic)
                {
                    /* Hermite: p0, its out tangent, p1, its in tangent (scaled by the span). */
                    const float T2 = T * T, T3 = T2 * T;
                    const float H00 = 2 * T3 - 3 * T2 + 1, H10 = T3 - 2 * T2 + T, H01 = -2 * T3 + 3 * T2, H11 = T3 - T2;
                    for(int C = 0; C < Components; ++C)
                    {
                        OutValue[C] = H00 * Value(A)[C] + H10 * Span * Value(A, 2)[C] + H01 * Value(B)[C] + H11 * Span * Value(B, 0)[C];
                    }
                }
                else if(Components == 4)
                {
                    const RenderCore::AnimQuat QA{ Value(A)[0], Value(A)[1], Value(A)[2], Value(A)[3] };
                    const RenderCore::AnimQuat QB{ Value(B)[0], Value(B)[1], Value(B)[2], Value(B)[3] };
                    const RenderCore::AnimQuat Q = RenderCore::AnimMath::Slerp(QA, QB, T);
                    OutValue[0] = Q.X; OutValue[1] = Q.Y; OutValue[2] = Q.Z; OutValue[3] = Q.W;
                }
                else
                {
                    for(int C = 0; C < Components; ++C) { OutValue[C] = Value(A)[C] + (Value(B)[C] - Value(A)[C]) * T; }
                }
            }
        };

        template<typename T>
        bool KeysEqual(const T& InA, const T& InB);
        template<>
        bool KeysEqual(const RenderCore::AnimVec3& InA, const RenderCore::AnimVec3& InB)
        {
            return std::fabs(InA.X - InB.X) < 1e-4f && std::fabs(InA.Y - InB.Y) < 1e-4f && std::fabs(InA.Z - InB.Z) < 1e-4f;
        }
        template<>
        bool KeysEqual(const RenderCore::AnimQuat& InA, const RenderCore::AnimQuat& InB)
        {
            return std::fabs(RenderCore::AnimMath::Dot(InA, InB)) > 1.0f - 1e-7f;
        }

        /* A track whose keys are all the same is one key. */
        template<typename T>
        void Collapse(std::vector<T>& InOutKeys)
        {
            for(size_t At = 1; At < InOutKeys.size(); ++At)
            {
                if(!KeysEqual(InOutKeys[At], InOutKeys[0])) { return; }
            }
            InOutKeys.resize(std::min<size_t>(InOutKeys.size(), 1));
        }

        /* InAnimation resampled at InOptions.AnimationSampleRate onto the
         * skeleton's bones. False (warned) if it moves none of them. */
        bool BuildClip(const cgltf_animation& InAnimation, const SkeletonBuild& InBuild, const Conversion& InConvert,
                       const MeshImportOptions& InOptions, const QString& InName, RenderCore::AnimClipData& OutClip,
                       QStringList& OutWarnings)
        {
            /* Channels by bone; morph weights and nodes off the skeleton are not animated. */
            std::vector<std::vector<ChannelKeys>> ByBone(InBuild.Skeleton.Bones.size());
            float Start = 0.0f, End = 0.0f;
            bool  bAny = false, bSkipped = false;
            for(cgltf_size At = 0; At < InAnimation.channels_count; ++At)
            {
                const cgltf_animation_channel& Channel = InAnimation.channels[At];
                const int32_t Bone = InBuild.Find(Channel.target_node);
                const bool bTRS = Channel.target_path == cgltf_animation_path_type_translation
                               || Channel.target_path == cgltf_animation_path_type_rotation
                               || Channel.target_path == cgltf_animation_path_type_scale;
                ChannelKeys Keys;
                if(Bone < 0 || !bTRS || !Keys.Read(Channel))
                {
                    bSkipped = true;
                    continue;
                }
                Start = bAny ? std::min(Start, Keys.Times.front()) : Keys.Times.front();
                End   = bAny ? std::max(End, Keys.Times.back())   : Keys.Times.back();
                bAny  = true;
                ByBone[size_t(Bone)].push_back(std::move(Keys));
            }
            if(bSkipped)
            {
                OutWarnings << QStringLiteral("%1: channels that move no bone (morph weights, or nodes off the skeleton) are left out").arg(InName);
            }
            if(!bAny)
            {
                OutWarnings << QStringLiteral("%1: skipped -- it moves no bone").arg(InName);
                return false;
            }

            OutClip = RenderCore::AnimClipData();
            OutClip.SampleRate = InOptions.AnimationSampleRate;
            OutClip.FrameCount = uint32_t(std::lround(double(End - Start) * OutClip.SampleRate)) + 1;
            OutClip.FrameCount = std::min<uint32_t>(OutClip.FrameCount, 1u << 20);

            for(size_t Bone = 0; Bone < ByBone.size(); ++Bone)
            {
                if(ByBone[Bone].empty()) { continue; }

                /* What a channel does not move stays at the node's own value. */
                const RenderCore::AnimTransform Rest = NodeLocal(*InBuild.Nodes[Bone]);

                RenderCore::AnimTrack Track;
                Track.Bone = InBuild.Skeleton.Bones[Bone].Name;
                for(uint32_t Frame = 0; Frame < OutClip.FrameCount; ++Frame)
                {
                    const float Time = std::min(End, Start + float(Frame) / OutClip.SampleRate);
                    RenderCore::AnimTransform Local = Rest;
                    for(const ChannelKeys& Keys : ByBone[Bone])
                    {
                        float Value[4] = {0.0f, 0.0f, 0.0f, 1.0f};
                        Keys.Sample(Time, Value);
                        switch(Keys.Channel->target_path)
                        {
                            case cgltf_animation_path_type_translation: Local.Translation = { Value[0], Value[1], Value[2] }; break;
                            case cgltf_animation_path_type_rotation:
                                Local.Rotation = RenderCore::AnimMath::Normalize({ Value[0], Value[1], Value[2], Value[3] });
                                break;
                            case cgltf_animation_path_type_scale:       Local.Scale = { Value[0], Value[1], Value[2] }; break;
                            default: break;
                        }
                    }
                    const RenderCore::AnimTransform Converted = InConvert.Local(Local);
                    Track.Translations.push_back(Converted.Translation);
                    Track.Rotations.push_back(Converted.Rotation);
                    Track.Scales.push_back(Converted.Scale);
                }
                Collapse(Track.Translations);
                Collapse(Track.Rotations);
                Collapse(Track.Scales);

                /* The topmost bone that travels is the one root motion means. */
                if(OutClip.RootBone.empty() && Track.Translations.size() > 1) { OutClip.RootBone = Track.Bone; }
                OutClip.Tracks.push_back(std::move(Track));
            }
            if(OutClip.RootBone.empty() && !InBuild.Skeleton.Bones.empty())
            {
                OutClip.RootBone = InBuild.Skeleton.Bones.front().Name;
            }
            return true;
        }

        /* One primitive onto InOutMesh as a new section. False (with a
         * warning) if it is skipped. */
        bool AppendPrimitive(RenderCore::MeshData& InOutMesh, const cgltf_primitive& InPrimitive, const Transform& InTransform,
                             const MeshImportOptions& InOptions, const QString& InSectionName, const PrimitiveSkin* InSkin,
                             QStringList& OutWarnings)
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
            const cgltf_accessor* UV1s      = FindAttribute(InPrimitive, cgltf_attribute_type_texcoord, 1);
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

            /* glTF (right-handed) -> engine (left-handed): x and y stay, z flips. */
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
                InOutMesh.Positions.push_back(Placed[1] * InOptions.Scale);
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
                InOutMesh.Normals.push_back(Normal[1]);
                InOutMesh.Normals.push_back(Normal[2] * Flip);

                float UV[2] = {0.0f, 0.0f};
                if(UVs) { cgltf_accessor_read_float(UVs, Vertex, UV, 2); }
                InOutMesh.UV0.push_back(UV[0]);
                InOutMesh.UV0.push_back(UV[1]);

                /* Lightmap UVs (TEXCOORD_1) only from primitives that have
                 * them: if any lacks them the count falls short, and LoadGltf
                 * generates the mesh's instead. */
                if(UV1s)
                {
                    float UV1[2] = {0.0f, 0.0f};
                    cgltf_accessor_read_float(UV1s, Vertex, UV1, 2);
                    InOutMesh.UV1.push_back(UV1[0]);
                    InOutMesh.UV1.push_back(UV1[1]);
                }

                if(!InOutMesh.Colors.empty())
                {
                    /* Float or normalised integers alike; a vec3 colour keeps alpha 1. */
                    float Color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
                    if(Colors) { cgltf_accessor_read_float(Colors, Vertex, Color, 4); }
                    InOutMesh.Colors.insert(InOutMesh.Colors.end(), Color, Color + 4);
                }

                if(InSkin)
                {
                    uint16_t Joints[4];
                    float    Weights[4];
                    ReadInfluences(InPrimitive, *InSkin, Vertex, Joints, Weights);
                    InOutMesh.Joints.insert(InOutMesh.Joints.end(), Joints, Joints + 4);
                    InOutMesh.Weights.insert(InOutMesh.Weights.end(), Weights, Weights + 4);
                }
            }

            /* A mirror flips winding -- the node's transform, the axis
             * conversion, or both (which cancel); swap two corners to undo it. */
            const bool bSwap = InTransform.bMirrors != InOptions.bConvertCoordinates;
            const size_t FirstIndex = InOutMesh.Indices.size();
            for(cgltf_size At = 0; At < IndexCount; At += 3)
            {
                InOutMesh.Indices.push_back(BaseVertex + Local[At]);
                InOutMesh.Indices.push_back(BaseVertex + Local[bSwap ? At + 2 : At + 1]);
                InOutMesh.Indices.push_back(BaseVertex + Local[bSwap ? At + 1 : At + 2]);
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
                        const Transform& InTransform, const MeshImportOptions& InOptions, const PrimitiveSkin* InSkin,
                        QStringList& OutWarnings)
        {
            for(cgltf_size At = 0; At < InMesh.primitives_count; ++At)
            {
                const QString Section = InMesh.primitives_count > 1 ? QStringLiteral("%1.%2").arg(InName).arg(At) : InName;
                AppendPrimitive(InOutMesh, InMesh.primitives[At], InTransform, InOptions, Section, InSkin, OutWarnings);
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
                  QList<ReferencedImage>& OutImages, QStringList& OutWarnings, QString& OutError,
                  GltfAnimationSet* OutAnimation)
    {
        GltfFile File;
        if(!File.Load(InPath, OutError))
        {
            return false;
        }
        const cgltf_data& Data = *File.Data;

        /* The skeleton first: which meshes are skinned depends on it. */
        const Conversion Convert(InOptions);
        SkeletonBuild Skeleton;
        const bool bSkeleton = OutAnimation && InOptions.bImportAnimations && BuildSkeleton(Data, Convert, Skeleton, OutWarnings);
        bool bSkinnedNode = false;
        for(cgltf_size At = 0; bSkeleton && At < Data.nodes_count; ++At)
        {
            bSkinnedNode = bSkinnedNode || (Data.nodes[At].skin && Data.nodes[At].mesh);
        }

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
                /* A skinned mesh is in bind space: glTF ignores its node's
                 * transform, and its bones place it. Combined with skinned
                 * ones, an unskinned mesh rides its nearest bone rigidly. */
                const bool bSkinned = bSkeleton && InNode.skin;
                PrimitiveSkin Skin;
                Skin.Build     = &Skeleton;
                Skin.Skin      = bSkinned ? InNode.skin : nullptr;
                Skin.RigidBone = bSkeleton ? Skeleton.FindNearest(&InNode) : 0;
                const bool bBind = bSkinned || (bSkinnedNode && InOptions.bCombineMeshes);

                Transform Placement;
                if(bSkinned)
                {
                    std::fill(std::begin(Placement.M), std::end(Placement.M), 0.0f);
                    Placement.M[0] = Placement.M[5] = Placement.M[10] = Placement.M[15] = 1.0f;
                }
                else
                {
                    cgltf_node_transform_world(&InNode, Placement.M);
                }
                Placement.Prepare();
                const QString Name = NameOf(InNode.mesh->name, NameOf(InNode.name, QStringLiteral("Mesh%1").arg(MeshesPlaced)));
                AppendMesh(MeshFor(Name), *InNode.mesh, Name, Placement, InOptions, bBind ? &Skin : nullptr, OutWarnings);
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
                AppendMesh(MeshFor(Name), Data.meshes[At], Name, Identity, InOptions, nullptr, OutWarnings);
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
        for(GltfMesh& Each : OutMeshes)
        {
            RenderCore::MeshData& Mesh = Each.Data;
            if(Mesh.UV1.size() != size_t(Mesh.GetVertexCount()) * 2)
            {
                /* A skinned mesh moves: never lightmapped, so no seams cut for it. */
                Mesh.UV1.clear();
                std::string Error;
                if(InOptions.bGenerateLightmapUVs && !Mesh.IsSkinned() && !GenerateLightmapUVs(Mesh, Error))
                {
                    OutWarnings << QStringLiteral("%1: no lightmap UVs -- %2").arg(Each.Name, QString::fromStdString(Error));
                }
            }
            RenderCore::ComputeMeshBounds(Mesh);
            if(Mesh.IsSkinned())
            {
                Mesh.InverseBind = Skeleton.InverseBind;
                RenderCore::ComputeSkinnedMeshBounds(Mesh, Skeleton.Skeleton);
            }
        }

        if(bSkeleton)
        {
            OutAnimation->bHasSkeleton = true;
            OutAnimation->Skeleton     = Skeleton.Skeleton;
            OutAnimation->Clips.clear();
            std::set<QString> ClipNames;
            for(cgltf_size At = 0; At < Data.animations_count; ++At)
            {
                const QString Base = NameOf(Data.animations[At].name, QStringLiteral("Anim%1").arg(At));
                QString Unique = Base;
                for(int Suffix = 2; ClipNames.count(Unique) != 0; ++Suffix) { Unique = QStringLiteral("%1_%2").arg(Base).arg(Suffix); }
                GltfClip Clip;
                Clip.Name = Unique;
                if(BuildClip(Data.animations[At], Skeleton, Convert, InOptions, Unique, Clip.Data, OutWarnings))
                {
                    ClipNames.insert(Unique);
                    OutAnimation->Clips << Clip;
                }
            }
        }

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
