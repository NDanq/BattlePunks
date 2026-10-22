#if WITH_EDITOR

#include "Private/Editor/Content/MeshTangents.h"

#include <cmath>
#include <cstring>

#include "mikktspace.h"

namespace EditorContent
{
    namespace
    {
        /* What MikkTSpace's callbacks read, and the frame it gives each triangle corner. */
        struct TangentJob
        {
            const RenderCore::MeshData* Mesh = nullptr;
            std::vector<float>          Corners;   // 4 per corner, as MeshData::Tangents
        };

        const TangentJob& JobOf(const SMikkTSpaceContext* InContext) { return *static_cast<const TangentJob*>(InContext->m_pUserData); }

        uint32_t IndexOf(const SMikkTSpaceContext* InContext, const int InFace, const int InVert)
        {
            return JobOf(InContext).Mesh->Indices[size_t(InFace) * 3 + size_t(InVert)];
        }

        int GetNumFaces(const SMikkTSpaceContext* InContext) { return int(JobOf(InContext).Mesh->GetTriangleCount()); }

        int GetNumVerticesOfFace(const SMikkTSpaceContext*, const int) { return 3; }

        void GetPosition(const SMikkTSpaceContext* InContext, float OutPosition[], const int InFace, const int InVert)
        {
            std::memcpy(OutPosition, &JobOf(InContext).Mesh->Positions[size_t(IndexOf(InContext, InFace, InVert)) * 3], 3 * sizeof(float));
        }

        void GetNormal(const SMikkTSpaceContext* InContext, float OutNormal[], const int InFace, const int InVert)
        {
            std::memcpy(OutNormal, &JobOf(InContext).Mesh->Normals[size_t(IndexOf(InContext, InFace, InVert)) * 3], 3 * sizeof(float));
        }

        void GetTexCoord(const SMikkTSpaceContext* InContext, float OutUV[], const int InFace, const int InVert)
        {
            std::memcpy(OutUV, &JobOf(InContext).Mesh->UV0[size_t(IndexOf(InContext, InFace, InVert)) * 2], 2 * sizeof(float));
        }

        void SetTSpaceBasic(const SMikkTSpaceContext* InContext, const float InTangent[], const float InSign, const int InFace,
                            const int InVert)
        {
            float* Out = &static_cast<TangentJob*>(InContext->m_pUserData)->Corners[(size_t(InFace) * 3 + size_t(InVert)) * 4];
            Out[0] = InTangent[0];
            Out[1] = InTangent[1];
            Out[2] = InTangent[2];
            Out[3] = InSign < 0.0f ? -1.0f : 1.0f;
        }

        bool SameTangent(const float* InA, const float* InB)
        {
            return InA[3] == InB[3] && std::fabs(InA[0] - InB[0]) < 1e-4f && std::fabs(InA[1] - InB[1]) < 1e-4f
                   && std::fabs(InA[2] - InB[2]) < 1e-4f;
        }

        /* InStream's values for vertex InSource, appended: a split vertex's copy. */
        template<typename T>
        void AppendCopy(std::vector<T>& InOutStream, const uint32_t InSource, const size_t InWidth)
        {
            if(InOutStream.empty()) { return; }
            const size_t From = size_t(InSource) * InWidth;
            for(size_t At = 0; At < InWidth; ++At) { InOutStream.push_back(InOutStream[From + At]); }
        }
    }

    bool GenerateMeshTangents(RenderCore::MeshData& InOutMesh)
    {
        const uint32_t VertexCount = InOutMesh.GetVertexCount();
        if(InOutMesh.Indices.empty() || InOutMesh.Normals.size() != size_t(VertexCount) * 3
           || InOutMesh.UV0.size() != size_t(VertexCount) * 2)
        {
            return false;
        }

        TangentJob Job;
        Job.Mesh = &InOutMesh;
        Job.Corners.assign(InOutMesh.Indices.size() * 4, 0.0f);

        SMikkTSpaceInterface Interface{};
        Interface.m_getNumFaces          = GetNumFaces;
        Interface.m_getNumVerticesOfFace = GetNumVerticesOfFace;
        Interface.m_getPosition          = GetPosition;
        Interface.m_getNormal            = GetNormal;
        Interface.m_getTexCoord          = GetTexCoord;
        Interface.m_setTSpaceBasic       = SetTSpaceBasic;
        SMikkTSpaceContext Context{};
        Context.m_pInterface = &Interface;
        Context.m_pUserData  = &Job;
        if(!genTangSpaceDefault(&Context))
        {
            return false;
        }

        /* Each corner's frame onto its vertex; a vertex whose corners disagree
         * gets a copy per frame. NextCopy chains a vertex to its copies. */
        RenderCore::MeshData& Mesh = InOutMesh;
        Mesh.Tangents.assign(size_t(VertexCount) * 4, 0.0f);
        std::vector<bool>     bSet(VertexCount, false);
        std::vector<uint32_t> NextCopy(VertexCount, UINT32_MAX);
        for(size_t Corner = 0; Corner < Mesh.Indices.size(); ++Corner)
        {
            const float*   Frame  = &Job.Corners[Corner * 4];
            const uint32_t Vertex = Mesh.Indices[Corner];
            if(!bSet[Vertex])
            {
                bSet[Vertex] = true;
                std::memcpy(&Mesh.Tangents[size_t(Vertex) * 4], Frame, 4 * sizeof(float));
                continue;
            }

            uint32_t Match = Vertex;
            uint32_t Last  = Vertex;
            while(Match != UINT32_MAX && !SameTangent(&Mesh.Tangents[size_t(Match) * 4], Frame))
            {
                Last  = Match;
                Match = NextCopy[Match];
            }
            if(Match == UINT32_MAX)
            {
                Match = Mesh.GetVertexCount();
                AppendCopy(Mesh.Positions, Vertex, 3);
                AppendCopy(Mesh.Normals,   Vertex, 3);
                AppendCopy(Mesh.UV0,       Vertex, 2);
                AppendCopy(Mesh.Colors,    Vertex, 4);
                AppendCopy(Mesh.UV1,       Vertex, 2);
                AppendCopy(Mesh.Joints,    Vertex, 4);   // a skinned mesh's copies keep their bones
                AppendCopy(Mesh.Weights,   Vertex, 4);
                Mesh.Tangents.insert(Mesh.Tangents.end(), Frame, Frame + 4);
                bSet.push_back(true);
                NextCopy.push_back(UINT32_MAX);
                NextCopy[Last] = Match;
            }
            Mesh.Indices[Corner] = Match;
        }
        return true;
    }
}

#endif // WITH_EDITOR
