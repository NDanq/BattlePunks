#ifndef CORECLIENT_D3D12MESH_H
#define CORECLIENT_D3D12MESH_H

#include <d3d12.h>
#include <wrl/client.h>

#include "Public/GDI/GDIMesh.h"

class D3D12Device;
class D3D12CurrentStateData;

/**
 * Position (slot 0) and texcoord (slot 1) live in default-heap buffers.
 * Instanced meshes add persistently mapped upload rings for transform (slot 2),
 * opacity (slot 3) and colour (slot 4), advanced in lockstep.
 */
class D3D12Mesh : public GDIMesh
{
public:
    D3D12Mesh(D3D12Device* InDevice, D3D12CurrentStateData* InStateData);
    ~D3D12Mesh() override;

    void InitResource() override;
    void Release() override;
    void Draw() override;

    /** Names each of its buffers, "<name> Positions" and so on. */
    void SetDebugName(const std::string& InName) override;

    /**
     * Make this a dynamic mesh (GDICreateDynamicMesh) instead: the indexed
     * layout's buffers in the upload heap, persistently mapped and written in
     * place by UpdateDynamic. False (nothing kept) on failure.
     */
    bool InitDynamic(uint32_t InMaxVertices, uint32_t InMaxIndices);

protected:
    /** memcpy into the mapped buffers; nothing is in flight there (GDIMesh::UpdateDynamic). */
    bool WriteDynamic(const GDIMeshDesc& InDesc, uint32_t InBaseVertex, uint32_t InBaseIndex) override;

    /**
     * The rings are frame-scoped: draws recorded earlier in the frame read their
     * slots at execution time, so each batch appends behind the last and Draw()
     * offsets into it with StartInstanceLocation. An exhausted ring flushes the
     * frame before rewinding.
     */
    void AddInstancedTransformInner() override;
    void ResetInstancedTransformInner() override;

private:
    struct RingBuffer
    {
        Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
        void*                                  Mapped = nullptr;
        UINT                                   Stride = 0;
    };

    bool CreateRing(RingBuffer& OutRing, UINT InStride);

    /** SetDebugName's names, on whichever buffers exist. */
    void ApplyDebugName() const;
    void BindRing(ID3D12GraphicsCommandList* InList, UINT InSlot, const RingBuffer& InRing) const;

    D3D12Device*                            Device    = nullptr;
    D3D12CurrentStateData*                  StateData = nullptr;

    Microsoft::WRL::ComPtr<ID3D12Resource>  PositionBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource>  TexCoordBuffer;
    UINT                                    PositionBytes = 0;
    UINT                                    TexCoordBytes = 0;

    // Indexed meshes only (GDICreateMesh): normals in slot 2, uint32 indices.
    Microsoft::WRL::ComPtr<ID3D12Resource>  NormalBuffer;
    Microsoft::WRL::ComPtr<ID3D12Resource>  ColorBuffer;     // float4 per vertex, slot 3
    Microsoft::WRL::ComPtr<ID3D12Resource>  UV1Buffer;       // float2 per vertex, slot 4
    Microsoft::WRL::ComPtr<ID3D12Resource>  JointBuffer;     // float4 per vertex, slot 6; skinned only
    Microsoft::WRL::ComPtr<ID3D12Resource>  WeightBuffer;    // float4 per vertex, slot 7; skinned only
    Microsoft::WRL::ComPtr<ID3D12Resource>  IndexBuffer;
    UINT                                    IndexBytes    = 0;

    // A dynamic mesh's mappings: position, texcoord, normal, colour, UV1, index.
    void*                                   DynamicMapped[6] = {};

    RingBuffer                              TransformRing;
    RingBuffer                              OpacityRing;
    RingBuffer                              ColorRing;

    static constexpr int InstanceRingBatches = 16;
    int                  RingCursor = 0;
    int                  BatchStart = 0;

    bool                 bResourcesValid = false;
};

#endif //CORECLIENT_D3D12MESH_H
