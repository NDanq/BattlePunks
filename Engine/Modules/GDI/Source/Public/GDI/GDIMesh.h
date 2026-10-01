#ifndef CORECLIENT_GDIMESH_H
#define CORECLIENT_GDIMESH_H


#include "Public/Math/Vector.h"
#include "Public/Math/Matrix.h"
#include <cstdint>
#include <vector>

/*
 * Input to GDICreateMesh: per-vertex position, normal and UV, plus a triangle
 * index list. Copied. Fixed attribute locations on every backend:
 *
 *   location 0  vec3 position
 *   location 1  vec2 texcoord
 *   location 8  vec3 normal        (indexed only; 2-7 are instancing's)
 */
struct GDIMeshDesc
{
    const float*    Positions   = nullptr;
    const float*    Normals     = nullptr;
    const float*    UV0         = nullptr;
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

    virtual void Release() = 0;

    virtual void Draw() = 0;

    inline bool     IsIndexed()     const { return RenderData.Indices != nullptr && RenderData.NumIndices > 0; }
    inline uint32_t GetNumIndices() const { return RenderData.NumIndices; }

    /* Index range for the next Draw() of an indexed mesh (clamped; count 0 =
     * all), so sections can draw with different shaders. */
    inline void SetDrawRange(const uint32_t InFirst, const uint32_t InCount)
    {
        DrawFirstIndex = InFirst;
        DrawIndexCount = InCount;
    }

protected:
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
        free(RenderData.Indices);  RenderData.Indices  = nullptr;
    }

    EPrimitiveType  PrimitiveType;
    ORenderData     RenderData;
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
