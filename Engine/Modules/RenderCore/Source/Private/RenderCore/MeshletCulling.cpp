#include "Private/RenderCore/MeshletCulling.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "Private/RenderCore/RenderCoreInternal.h"
#include "Public/RenderCore/MeshAsset.h"

namespace RenderCore
{
    namespace
    {
        /* MeshletCull.csh's workgroup: one meshlet a thread. */
        constexpr uint32_t CULL_GROUP_SIZE = 64;

        /* MeshletCull.csh's MeshletCullParams (std140). */
        struct CullParams
        {
            float    Planes[6][4];   // the view's, world space, inward: dot(N, P) + D >= 0 inside
            float    Eye[4];         // xyz: the view's position; w: 1 to cull by the normal cones
            uint32_t Layout[4];      // first record, records, vertex base, triangle base
            float    HzbRows[4][4];  // the depth pyramid's view projection, by row
            uint32_t Hzb[4];         // levels (0: no pyramid), the first level's width and height
            uint32_t HzbLevels[MESHLET_HZB_LEVELS_MAX][4];   // each level's offset (floats), width, height
        };
        static_assert(sizeof(CullParams) == 464, "MeshletCull.csh's MeshletCullParams");

        /* MeshletHzbFirst.csh's and MeshletHzbDown.csh's MeshletHzbParams (std140). */
        struct HzbParams
        {
            uint32_t Source[4];        // width, height, offset (floats; the first: unused)
            uint32_t Destination[4];   // width, height, offset (floats)
        };

        /* The pyramid's workgroups: 8 x 8 texels. */
        constexpr uint32_t HZB_GROUP_SIZE = 8;

        uint32_t FloatBits(const float InValue)
        {
            uint32_t Bits = 0;
            std::memcpy(&Bits, &InValue, sizeof(Bits));
            return Bits;
        }
    }

    bool MeshletHzb::Build(GDIShader* InFirst, GDIShader* InDown, GDITextureResource* InDepth, const uint32_t InWidth,
                           const uint32_t InHeight, const EngineCore::Matrix4x4& InViewProjection)
    {
        bValid = false;
        if(!InFirst || !InDown || !InDepth || InWidth == 0 || InHeight == 0)
        {
            return false;
        }

        /* Half the drawn size each way, rounded up, then halving to 1 x 1. */
        uint32_t Floats = 0;
        Levels = 0;
        uint32_t Width = (InWidth + 1) / 2, Height = (InHeight + 1) / 2;
        while(Levels < MESHLET_HZB_LEVELS_MAX)
        {
            Offsets[Levels] = Floats;
            Size[Levels][0] = Width;
            Size[Levels][1] = Height;
            Floats += Width * Height;
            ++Levels;
            if(Width == 1 && Height == 1) { break; }
            Width  = (Width + 1) / 2;
            Height = (Height + 1) / 2;
        }
        const uint32_t Bytes = Floats * uint32_t(sizeof(float));
        if(!Buffer || Buffer->GetSize() < Bytes)
        {
            if(Buffer) { Buffer->Release(); }
            Buffer = GDICreateBuffer(Bytes, GDI_BU_Storage);
            GDISetDebugName(Buffer, "RenderCore.Meshlets.DepthPyramid");
            if(!Buffer) { return false; }
        }

        SCOPED_DRAW_EVENT("MeshletDepthPyramid");
        GDIBuffer* Buffers[1] = { Buffer };
        for(uint32_t Level = 0; Level < Levels; ++Level)
        {
            HzbParams Params{};
            Params.Source[0]      = Level == 0 ? InWidth : Size[Level - 1][0];
            Params.Source[1]      = Level == 0 ? InHeight : Size[Level - 1][1];
            Params.Source[2]      = Level == 0 ? 0u : Offsets[Level - 1];
            Params.Destination[0] = Size[Level][0];
            Params.Destination[1] = Size[Level][1];
            Params.Destination[2] = Offsets[Level];
            const uint32_t X = (Size[Level][0] + HZB_GROUP_SIZE - 1) / HZB_GROUP_SIZE;
            const uint32_t Y = (Size[Level][1] + HZB_GROUP_SIZE - 1) / HZB_GROUP_SIZE;
            const bool bDone = Level == 0
                             ? GDIDispatchCompute(InFirst, &Params, uint32_t(sizeof(Params)), Buffers, 1, InDepth, X, Y)
                             : GDIDispatchCompute(InDown, &Params, uint32_t(sizeof(Params)), Buffers, 1, X, Y);
            if(!bDone) { return false; }
        }
        ViewProjection = InViewProjection;
        bValid = true;
        return true;
    }

    void MeshletHzb::Release()
    {
        if(Buffer) { Buffer->Release(); }   // deferred past any frame still reading it
        Buffer = nullptr;
        Levels = 0;
        bValid = false;
    }

    MeshletGpuMesh::~MeshletGpuMesh()
    {
        if(Data) { Data->Release(); }   // deferred past any frame still reading it
    }

    std::unique_ptr<MeshletGpuMesh> MeshletGpuMesh::Create(const MeshData& InMesh, const std::string& InName)
    {
        if(InMesh.IsSkinned() || !GDISupportsMeshletCulling())
        {
            return nullptr;
        }

        /* A mesh without them (a primitive, a mesh made in memory) builds its own, if it gains by it. */
        const MeshData* Source = &InMesh;
        MeshData Built;
        if(!InMesh.HasMeshlets())
        {
            if(InMesh.GetTriangleCount() < 2 * MESHLET_MAX_TRIANGLES)
            {
                return nullptr;
            }
            Built.Positions = InMesh.Positions;
            Built.Indices   = InMesh.Indices;
            Built.Sections  = InMesh.Sections;
            if(Built.Sections.empty())
            {
                MeshSection All;
                All.IndexCount = uint32_t(Built.Indices.size());
                Built.Sections.push_back(All);
            }
            if(!BuildMeshlets(Built))
            {
                return nullptr;
            }
            Source = &Built;
        }
        else if(InMesh.Sections.empty())
        {
            return nullptr;   // drawn as one section of everything: built against sections it does not have
        }

        /* Each section's meshlets must be one run (BuildMeshlets makes them so). */
        auto Out = std::make_unique<MeshletGpuMesh>();
        Out->SectionFirst.assign(Source->Sections.size(), 0u);
        Out->SectionCount.assign(Source->Sections.size(), 0u);
        for(uint32_t Index = 0; Index < Source->Meshlets.size(); ++Index)
        {
            const uint32_t Section = Source->Meshlets[Index].Section;
            if(Out->SectionCount[Section] == 0)
            {
                Out->SectionFirst[Section] = Index;
            }
            else if(Out->SectionFirst[Section] + Out->SectionCount[Section] != Index)
            {
                return nullptr;
            }
            ++Out->SectionCount[Section];
        }

        const uint32_t MeshletCount = uint32_t(Source->Meshlets.size());
        Out->MeshletCount = MeshletCount;
        Out->VertexBase   = MeshletCount * MESHLET_GPU_WORDS;
        Out->TriangleBase = Out->VertexBase + uint32_t(Source->MeshletVertices.size());
        std::vector<uint32_t> Words(size_t(Out->TriangleBase) + (Source->MeshletTriangles.size() + 3) / 4, 0u);
        for(uint32_t Index = 0; Index < MeshletCount; ++Index)
        {
            const MeshMeshlet& Meshlet = Source->Meshlets[Index];
            uint32_t* At = &Words[size_t(Index) * MESHLET_GPU_WORDS];
            At[0]  = Meshlet.VertexOffset;
            At[1]  = Meshlet.VertexCount;
            At[2]  = Meshlet.TriangleOffset;
            At[3]  = Meshlet.TriangleCount;
            At[4]  = FloatBits(Meshlet.Center[0]);
            At[5]  = FloatBits(Meshlet.Center[1]);
            At[6]  = FloatBits(Meshlet.Center[2]);
            At[7]  = FloatBits(Meshlet.Radius);
            At[8]  = FloatBits(Meshlet.ConeAxis[0]);
            At[9]  = FloatBits(Meshlet.ConeAxis[1]);
            At[10] = FloatBits(Meshlet.ConeAxis[2]);
            At[11] = FloatBits(Meshlet.ConeCutoff);
        }
        std::copy(Source->MeshletVertices.begin(), Source->MeshletVertices.end(), Words.begin() + Out->VertexBase);
        if(!Source->MeshletTriangles.empty())
        {
            std::memcpy(&Words[Out->TriangleBase], Source->MeshletTriangles.data(), Source->MeshletTriangles.size());   // little-endian, as the GPU reads it
        }

        Out->Data = GDICreateBuffer(uint32_t(Words.size() * sizeof(uint32_t)), GDI_BU_Storage, Words.data());
        if(!Out->Data)
        {
            return nullptr;
        }
        GDISetDebugName(Out->Data, "RenderCore.Meshlets." + InName);
        return Out;
    }

    void MeshletBatch::Release()
    {
        Reset();
        for(GDIBuffer** Each : { &RecordBuffer, &Indices, &Args })
        {
            if(*Each) { (*Each)->Release(); }   // deferred past any frame still reading it
            *Each = nullptr;
        }
    }

    void MeshletBatch::Reset()
    {
        Records.clear();
        Owners.clear();
        IndexTotal  = 0;
        Meshlets    = 0;
        bDispatched = false;
    }

    int32_t MeshletBatch::Add(MeshletGpuMesh* InMesh, const uint32_t InSection, const EngineCore::Matrix4x4& InModel, const float InScale,
                              const float InPad, const uint32_t InIndexCount)
    {
        if(!InMesh || InSection >= InMesh->SectionCount.size() || InMesh->SectionCount[InSection] == 0 || bDispatched)
        {
            return -1;
        }
        Record Each;
        for(int Row = 0; Row < 3; ++Row)
        {
            for(int Column = 0; Column < 4; ++Column) { Each.Model[Row][Column] = float(InModel.Data[Row][Column]); }
        }
        Each.MeshletFirst = InMesh->SectionFirst[InSection];
        Each.MeshletCount = InMesh->SectionCount[InSection];
        Each.IndexBase    = IndexTotal;
        Each.Slot         = uint32_t(Records.size());
        Each.Scale        = InScale;
        Each.Pad          = InPad;
        Records.push_back(Each);
        Owners.push_back(InMesh);
        IndexTotal += InIndexCount;
        Meshlets   += Each.MeshletCount;
        return int32_t(Each.Slot);
    }

    bool MeshletBatch::ReadDrawnIndices(uint64_t& OutIndices) const
    {
        OutIndices = 0;
        std::vector<uint8_t> Bytes;
        if(!bDispatched || !Args || !GDIReadBuffer(Args, Bytes) || Bytes.size() < Records.size() * GDI_INDIRECT_ARGS_STRIDE)
        {
            return false;
        }
        for(size_t Slot = 0; Slot < Records.size(); ++Slot)
        {
            uint32_t Count = 0;
            std::memcpy(&Count, &Bytes[Slot * GDI_INDIRECT_ARGS_STRIDE], sizeof(Count));
            OutIndices += Count;
        }
        return true;
    }

    bool MeshletBatch::Ensure(GDIBuffer*& InOut, const uint32_t InBytes, const uint8_t InUsage, const char* InName)
    {
        if(InOut && InOut->GetSize() >= InBytes)
        {
            return true;
        }
        if(InOut) { InOut->Release(); }
        InOut = GDICreateBuffer(InBytes + InBytes / 2, InUsage);
        GDISetDebugName(InOut, InName);
        return InOut != nullptr;
    }

    bool MeshletBatch::Dispatch(GDIShader* InCull, const EngineCore::Matrix4x4& InViewProjection, const EngineCore::Vector3& InEye,
                                const bool bInCones, const MeshletHzb* InHzb)
    {
        if(Records.empty() || !InCull)
        {
            return false;
        }

        /* Each mesh's records together: one dispatch reads one mesh's meshlets. */
        std::vector<uint32_t> Order(Records.size());
        for(uint32_t Index = 0; Index < Order.size(); ++Index) { Order[Index] = Index; }
        std::stable_sort(Order.begin(), Order.end(), [this](const uint32_t InA, const uint32_t InB) { return Owners[InA] < Owners[InB]; });
        std::vector<Record> Sorted(Records.size());
        for(uint32_t Index = 0; Index < Order.size(); ++Index) { Sorted[Index] = Records[Order[Index]]; }

        /* The arguments as nothing drawn: no indices yet, one instance, from its own region. */
        std::vector<uint32_t> Initial(Records.size() * 5, 0u);
        for(const Record& Each : Records)
        {
            Initial[size_t(Each.Slot) * 5 + 1] = 1;
            Initial[size_t(Each.Slot) * 5 + 2] = Each.IndexBase;
        }
        static_assert(GDI_INDIRECT_ARGS_STRIDE == 5 * sizeof(uint32_t), "an entry is five words");

        const uint32_t RecordBytes = uint32_t(Sorted.size() * sizeof(Record));
        const uint32_t ArgBytes    = uint32_t(Initial.size() * sizeof(uint32_t));
        if(!Ensure(RecordBuffer, RecordBytes, GDI_BU_Storage, "RenderCore.Meshlets.Records")
           || !Ensure(Indices, std::max(IndexTotal, 1u) * uint32_t(sizeof(uint32_t)), GDI_BU_Storage | GDI_BU_Index, "RenderCore.Meshlets.Indices")
           || !Ensure(Args, ArgBytes, GDI_BU_Storage | GDI_BU_Indirect, "RenderCore.Meshlets.Args")
           || !GDIUpdateBuffer(RecordBuffer, 0, Sorted.data(), RecordBytes)
           || !GDIUpdateBuffer(Args, 0, Initial.data(), ArgBytes))
        {
            return false;
        }

        /* The view's planes [Gribb and Hartmann 2001], inward and normalised.
         * Near is -w <= z, whichever depth range the projection maps to: with
         * 0 <= z that only keeps a little more, never culls what shows. */
        CullParams Params{};
        const auto Row = [&InViewProjection](const int InRow, const int InColumn) { return double(InViewProjection.Data[InRow][InColumn]); };
        const double Signs[6][2] = { { 0, 1 }, { 0, -1 }, { 1, 1 }, { 1, -1 }, { 2, 1 }, { 2, -1 } };
        for(int Plane = 0; Plane < 6; ++Plane)
        {
            double Equation[4];
            for(int Column = 0; Column < 4; ++Column)
            {
                Equation[Column] = Row(3, Column) + Signs[Plane][1] * Row(int(Signs[Plane][0]), Column);
            }
            const double Length = std::sqrt(Equation[0] * Equation[0] + Equation[1] * Equation[1] + Equation[2] * Equation[2]);
            for(int Column = 0; Column < 4; ++Column) { Params.Planes[Plane][Column] = Length > 0.0 ? float(Equation[Column] / Length) : 0.0f; }
        }
        Params.Eye[0] = float(InEye.GetX());
        Params.Eye[1] = float(InEye.GetY());
        Params.Eye[2] = float(InEye.GetZ());
        Params.Eye[3] = bInCones ? 1.0f : 0.0f;

        /* The last frame's depth pyramid; the shader declares a fifth buffer either way. */
        const bool bHzb = InHzb && InHzb->bValid && InHzb->Buffer;
        if(bHzb)
        {
            for(int Row = 0; Row < 4; ++Row)
            {
                for(int Column = 0; Column < 4; ++Column) { Params.HzbRows[Row][Column] = float(InHzb->ViewProjection.Data[Row][Column]); }
            }
            Params.Hzb[0] = InHzb->Levels;
            Params.Hzb[1] = InHzb->Size[0][0];
            Params.Hzb[2] = InHzb->Size[0][1];
            for(uint32_t Level = 0; Level < InHzb->Levels; ++Level)
            {
                Params.HzbLevels[Level][0] = InHzb->Offsets[Level];
                Params.HzbLevels[Level][1] = InHzb->Size[Level][0];
                Params.HzbLevels[Level][2] = InHzb->Size[Level][1];
            }
        }
        GDIBuffer* Pyramid = bHzb ? InHzb->Buffer : RecordBuffer;

        SCOPED_DRAW_EVENT("MeshletCull");
        for(uint32_t First = 0; First < Order.size();)
        {
            MeshletGpuMesh* Mesh = Owners[Order[First]];
            uint32_t Count = 0, Widest = 0;
            while(First + Count < Order.size() && Owners[Order[First + Count]] == Mesh)
            {
                Widest = std::max(Widest, Sorted[First + Count].MeshletCount);
                ++Count;
            }
            Params.Layout[0] = First;
            Params.Layout[1] = Count;
            Params.Layout[2] = Mesh->VertexBase;
            Params.Layout[3] = Mesh->TriangleBase;
            GDIBuffer* Buffers[5] = { Mesh->Data, RecordBuffer, Indices, Args, Pyramid };
            if(!GDIDispatchCompute(InCull, &Params, uint32_t(sizeof(Params)), Buffers, 5,
                                   (Widest + CULL_GROUP_SIZE - 1) / CULL_GROUP_SIZE, Count))
            {
                return false;
            }
            First += Count;
        }
        bDispatched = true;
        return true;
    }
}
