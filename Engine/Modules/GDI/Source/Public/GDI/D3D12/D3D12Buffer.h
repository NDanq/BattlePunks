#ifndef CORECLIENT_D3D12BUFFER_H
#define CORECLIENT_D3D12BUFFER_H

#include <d3d12.h>
#include <wrl/client.h>

#include <cstdint>

#include "Public/GDI/GDIBuffer.h"

class D3D12Device;

/**
 * D3D12 GDIBuffer: one DEFAULT-heap buffer with unordered access, read and
 * written by compute (raw UAVs u1..), read by instanced draws as a vertex
 * buffer and by indirect draws as their arguments. Filled once at creation by
 * a synchronous upload copy, or zeroed; after that only shaders write it.
 *
 * Its state is tracked per command list: a buffer decays to COMMON at the end
 * of every ExecuteCommandLists, so a state recorded in another list is COMMON
 * in this one (GetState).
 */
class D3D12Buffer final : public GDIBuffer
{
public:
    D3D12Buffer(D3D12Device* InDevice, uint32_t InBytes, uint8_t InUsage, const void* InInitialData);
    ~D3D12Buffer() override = default;

    /** Defers the free past the frames that may still read it, then deletes this. */
    void Release() override;

    /** Names its resource. */
    void SetDebugName(const std::string& InName) override;

    bool            IsValid()     const { return Resource != nullptr; }
    ID3D12Resource* GetResource() const { return Resource.Get(); }

    /** The read state its usage needs in a draw: vertex buffer, indirect arguments and/or index buffer. */
    D3D12_RESOURCE_STATES GetDrawReadState() const;

    /**
     * Records a transition to InTo in InList, the list InListSerial names
     * (D3D12DynamicGDI::GetListSerial); nothing if it is there already.
     */
    void Transition(ID3D12GraphicsCommandList* InList, uint64_t InListSerial, D3D12_RESOURCE_STATES InTo);

private:
    D3D12_RESOURCE_STATES GetState(const uint64_t InListSerial) const
    {
        return InListSerial == StateListSerial ? State : D3D12_RESOURCE_STATE_COMMON;
    }

    D3D12Device*                           Device = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> Resource;
    D3D12_RESOURCE_STATES                  State           = D3D12_RESOURCE_STATE_COMMON;
    uint64_t                               StateListSerial = 0;
};

#endif //CORECLIENT_D3D12BUFFER_H
