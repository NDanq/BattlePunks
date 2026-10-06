#ifndef CORECLIENT_GDIMESH_H
#define CORECLIENT_GDIMESH_H


#include "Public/Math/Vector.h"
#include "Public/Math/Matrix.h"
#include <cstdint>
#include <vector>

/*
 * Input to GDICreateMesh: per-vertex position, normal, UV and colour, plus a
 * triangle index list. Copied. Fixed attribute locations on every backend:
 *
 *   location 0  vec3 position
 *   location 1  vec2 texcoord
 *   location 8  vec3 normal        (indexed only; 2-7 are instancing's)
 *   location 9  vec4 colour        (indexed only; white when Colors is null)
 *   location 10 vec2 UV1           (indexed only; the lightmap UVs, zero when UV1 is null)
 */
struct GDIMeshDesc
{
    const float*    Positions   = nullptr;
    const float*    Normals     = nullptr;
    const float*    UV0         = nullptr;
    const float*    Colors      = nullptr;   // RGBA, 4 per vertex; optional
    const float*    UV1         = nullptr;   // 2 per vertex; optional
    uint32_t        NumVertices = 0;
    const uint32_t* Indices     = nullptr;
    uint32_t        NumIndices  = 0;
};

class GDIMesh
{
public:
    struct ORenderData
    {
        enum Buffers : char
        {
            VERTEX_BUFFER = 0,
            TEXCOORD_BUFFER,
            INSTANCED_TRANSFORM_BUFFER,
            INSTANCED_OPACITY_BUFFER,
            INSTANCED_COLOR_BUFFER,
            NUM_BUFFERS
        };
        float* Position = nullptr;
        float* TexCoord = nullptr;

        /* Indexed meshes only (GDICreateMesh); null for the quads. malloc'd,
         * freed by the backend's Release with the other two. */
        float*    Normal     = nullptr;
        float*    Color      = nullptr;   // RGBA, 4 per vertex
        float*    UV1        = nullptr;   // 2 per vertex, the lightmap UVs
        uint32_t* Indices    = nullptr;
        uint32_t  NumIndices = 0;

        uint NumVertices = 0;
        std::vector<float> PositionBuffer;
        std::vector<float> TexCoordBuffer;

        void AppendPositionData(const Vector3& InVector)
        {
            PositionBuffer.push_back(float(InVector.GetX()));
            PositionBuffer.push_back(float(InVector.GetY()));
            PositionBuffer.push_back(float(InVector.GetZ()));
        }

        void AppendTexCoordData(const Vector2& InVector)
        {
            TexCoordBuffer.push_back(float(InVector.GetX()));
            TexCoordBuffer.push_back(float(InVector.GetY()));
        }

        void Cleanup()
        {
            PositionBuffer.clear();
            TexCoordBuffer.clear();
        }
    };

    enum EPrimitiveType : char
    {
        PT_Point = 0,
        PT_MAX
    };

    enum EMeshType : char
    {
        MT_Static,
        MT_Stream,
        MT_MAX
    } MeshType;

    GDIMesh() = default;

    GDIMesh(const EMeshType& InType, const ORenderData& InData) :
        MeshType(InType),
        RenderData(InData)
    {

    }

    GDIMesh(const ORenderData& InData) :
        RenderData(InData)
    {

    }

    virtual ~GDIMesh(){  }

    void SetRenderData(ORenderData InRenderData)
    {
        RenderData = InRenderData;
    }

    inline void SetSupportsInstanced(bool bInValue)
    {
        bSupportsInstanced = bInValue;
    }

    inline bool IsInstanced() const { return bSupportsInstanced; }

    inline void AddInstancedTransform( Matrix4x4 InTransform, float InOpacity = 1.0f,
                                       Vector3 InColor = Vector3(1.0, 1.0, 1.0) )
    {
        if(!bSupportsInstanced) return;

        // Buffer full — silently dropping would desync NumInstanced from the GPU
        // buffer and make the draw read stale slots. Callers must flush (Draw +
        // ResetInstanced) and retry.
        if(NumInstanced >= MaxInstanced) return;

        NumInstanced++;
        InstancedTransforms.push_back(InTransform);
        // Parallel per-instance opacity stream; stays index-aligned with
        // InstancedTransforms and is uploaded to binding 3 / location 6.
        InstancedOpacities.push_back(InOpacity);
        // Parallel per-instance color stream; index-aligned with the transforms and
        // uploaded to binding 4 / location 7. Drives per-stamp RGB for pressure-
        // driven color transfer.
        InstancedColors.push_back(InColor);

        AddInstancedTransformInner();
    }

    inline int GetNumInstanced() const { return NumInstanced; }
    inline int GetMaxInstanced() const { return MaxInstanced; }

    inline void ResetInstanced()
    {
        NumInstanced = 0;
        InstancedTransforms.clear();
        InstancedOpacities.clear();
        InstancedColors.clear();

        ResetInstancedTransformInner();
    }

    virtual void InitResource() = 0;

    /* Its name in capture tools (GDISetDebugName): what RenderDoc, PIX and
     * Xcode show for its buffers. Kept, so a backend that makes them later
     * names them then. */
    virtual void SetDebugName(const std::string& InName) { DebugName = InName; }
    const std::string& GetDebugName() const { return DebugName; }

    virtual void Release() = 0;

    virtual void Draw() = 0;

    inline bool     IsIndexed()     const { return (RenderData.Indices != nullptr || bDynamic) && RenderData.NumIndices > 0; }
    inline uint32_t GetNumIndices() const { return RenderData.NumIndices; }

    /*
     * A dynamic mesh (GDICreateDynamicMesh): the GDIMeshDesc layout, rewritten
     * by UpdateDynamic rather than created once. Its GPU buffers hold
     * InMaxVertices / InMaxIndices and are written in place by the CPU, so it
     * keeps no RenderData copy. Within one BeginRendering block each update
     * APPENDS behind the last -- draws recorded earlier in the block still read
     * their own data at submit -- and the next block starts again at 0 (every
     * backend waits for the previous submission before recording). The draws
     * after an update read that update's data, by range (SetDrawRange) as for
     * any indexed mesh.
     *
     * False, with the mesh unchanged, if InDesc is invalid (as for CreateMesh;
     * Normals may be null) or no longer fits in this block: the caller makes a
     * bigger mesh, or waits for the next block.
     */
    inline bool     IsDynamic()                 const { return bDynamic; }
    inline uint32_t GetDynamicVertexCapacity()  const { return DynamicVertexCapacity; }
    inline uint32_t GetDynamicIndexCapacity()   const { return DynamicIndexCapacity; }
    bool            UpdateDynamic(const GDIMeshDesc& InDesc, uint64_t InRenderingSerial);   // GDI.cpp

    /* Index range for the next Draw() of an indexed mesh (clamped; count 0 =
     * all), so sections can draw with different shaders. */
    inline void SetDrawRange(const uint32_t InFirst, const uint32_t InCount)
    {
        DrawFirstIndex = InFirst;
        DrawIndexCount = InCount;
    }

protected:
    std::string DebugName;

    /* The range SetDrawRange asked for, clamped; Count 0 if there is nothing to draw. */
    inline void GetClampedDrawRange(uint32_t& OutFirst, uint32_t& OutCount) const
    {
        const uint32_t Total = RenderData.NumIndices;
        OutFirst = DrawFirstIndex < Total ? DrawFirstIndex : Total;
        const uint32_t Left = Total - OutFirst;
        OutCount = (DrawIndexCount == 0 || DrawIndexCount > Left) ? Left : DrawIndexCount;
    }

    /* Frees the malloc'd arrays in RenderData -- for each backend's Release. */
    inline void FreeRenderData()
    {
        free(RenderData.Position); RenderData.Position = nullptr;
        free(RenderData.TexCoord); RenderData.TexCoord = nullptr;
        free(RenderData.Normal);   RenderData.Normal   = nullptr;
        free(RenderData.Color);    RenderData.Color    = nullptr;
        free(RenderData.UV1);      RenderData.UV1      = nullptr;
        free(RenderData.Indices);  RenderData.Indices  = nullptr;
    }

    /* A backend's dynamic mesh calls this from its constructor, before
     * allocating the capacity in its own buffers. */
    inline void MakeDynamic(const uint32_t InMaxVertices, const uint32_t InMaxIndices)
    {
        bDynamic              = true;
        DynamicVertexCapacity = InMaxVertices;
        DynamicIndexCapacity  = InMaxIndices;
    }

    /* A dynamic mesh's write: InDesc (validated, fits) into its buffers at
     * vertex InBaseVertex and index InBaseIndex. Indices stay relative to the
     * update's own first vertex; Draw adds GetDynamicBaseVertex(). */
    virtual bool WriteDynamic(const GDIMeshDesc& /*InDesc*/, uint32_t /*InBaseVertex*/, uint32_t /*InBaseIndex*/) { return false; }

    /* Where the last update's vertices and indices start in the buffers. */
    inline uint32_t GetDynamicBaseVertex() const { return DynamicBaseVertex; }
    inline uint32_t GetDynamicBaseIndex()  const { return DynamicBaseIndex; }

    EPrimitiveType  PrimitiveType;
    ORenderData     RenderData;
    bool            bDynamic              = false;
    uint32_t        DynamicVertexCapacity = 0;
    uint32_t        DynamicIndexCapacity  = 0;
    uint32_t        DynamicBaseVertex     = 0;
    uint32_t        DynamicBaseIndex      = 0;
    uint32_t        DynamicNextVertex     = 0;
    uint32_t        DynamicNextIndex      = 0;
    uint64_t        DynamicSerial         = ~uint64_t(0);
    uint32_t        DrawFirstIndex = 0;
    uint32_t        DrawIndexCount = 0;
    bool            bSupportsInstanced = false;
    int             NumInstanced = 0;
    std::vector<Matrix4x4> InstancedTransforms;
    std::vector<float>     InstancedOpacities;
    std::vector<Vector3>   InstancedColors;
    const int        MaxInstanced = 512;
    virtual void AddInstancedTransformInner() = 0;
    virtual void ResetInstancedTransformInner() = 0;
};

#endif //CORECLIENT_GDIMESH_H
