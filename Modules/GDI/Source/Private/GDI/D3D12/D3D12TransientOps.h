#ifndef CORECLIENT_D3D12TRANSIENTOPS_H
#define CORECLIENT_D3D12TRANSIENTOPS_H

#include <d3d12.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>

#include "Public/GDI/D3D12/D3D12Device.h"
#include "Public/GDI/D3D12/D3D12DynamicGDI.h"
#include "Public/GDI/GDILog.h"

// One-shot GPU operations (uploads, readbacks) on their own command list.
// Readbacks wait; an upload into a fresh texture does not (UploadSubresources).

namespace D3D12Ops
{

inline Microsoft::WRL::ComPtr<ID3D12Resource> CreateBuffer(D3D12Device*          Device,
                                                           UINT64                Size,
                                                           D3D12_HEAP_TYPE       HeapType,
                                                           D3D12_RESOURCE_STATES InitialState)
{
    D3D12_HEAP_PROPERTIES Heap{};
    Heap.Type = HeapType;

    D3D12_RESOURCE_DESC Desc{};
    Desc.Dimension        = D3D12_RESOURCE_DIMENSION_BUFFER;
    Desc.Width            = Size;
    Desc.Height           = 1;
    Desc.DepthOrArraySize = 1;
    Desc.MipLevels        = 1;
    Desc.Format           = DXGI_FORMAT_UNKNOWN;
    Desc.SampleDesc.Count = 1;
    Desc.Layout           = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    Microsoft::WRL::ComPtr<ID3D12Resource> Buffer;
    if(FAILED(Device->GetDevice()->CreateCommittedResource(&Heap, D3D12_HEAP_FLAG_NONE, &Desc, InitialState,
                                                           nullptr, IID_PPV_ARGS(&Buffer))))
    {
        GDI_LOG_ERROR("[D3D12Ops] CreateCommittedResource failed for a buffer.")
    }
    return Buffer;
}

struct TransientCmd
{
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
};

inline bool BeginTransientCmd(D3D12Device* Device, TransientCmd& Out)
{
    ID3D12Device* Dev = Device->GetDevice();
    if(FAILED(Dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&Out.Allocator))) ||
       FAILED(Dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, Out.Allocator.Get(), nullptr, IID_PPV_ARGS(&Out.List))))
    {
        GDI_LOG_ERROR("[D3D12Ops] Failed to create a transient command list.")
        return false;
    }
    return true;
}

/**
 * Closes and executes Cmd. Without InAsync, waits for the device to go idle.
 * With it, does not wait: the list goes ahead of the frame being recorded on
 * the same queue, so it has finished before that frame's fence is signalled,
 * and InAsync keeps Cmd (and InStaging, a staging buffer of its own) until then.
 */
inline void EndAndSubmitTransientCmd(D3D12Device* Device, TransientCmd& Cmd, D3D12DynamicGDI* InAsync = nullptr,
                                     Microsoft::WRL::ComPtr<ID3D12Resource> InStaging = nullptr)
{
    Cmd.List->Close();
    {
        std::lock_guard<std::mutex> Lock(Device->GetQueueMutex());
        ID3D12CommandList* Lists[] = { Cmd.List.Get() };
        Device->GetQueue()->ExecuteCommandLists(1, Lists);
    }
    if(InAsync)
    {
        InAsync->DeferReleaseTransient(std::move(Cmd.Allocator), std::move(Cmd.List));
        if(InStaging) InAsync->DeferRelease(std::move(InStaging));
        return;
    }
    Device->WaitIdle();
}

inline void RecordTransition(ID3D12GraphicsCommandList* List, ID3D12Resource* Resource,
                             D3D12_RESOURCE_STATES From, D3D12_RESOURCE_STATES To)
{
    if(From == To || !Resource) return;

    D3D12_RESOURCE_BARRIER Barrier{};
    Barrier.Type                   = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    Barrier.Transition.pResource   = Resource;
    Barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    Barrier.Transition.StateBefore = From;
    Barrier.Transition.StateAfter  = To;
    List->ResourceBarrier(1, &Barrier);
}

inline bool UploadBuffer(D3D12Device* Device, ID3D12Resource* Dest, const void* Data, UINT64 Size)
{
    Microsoft::WRL::ComPtr<ID3D12Resource> Staging =
        CreateBuffer(Device, Size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
    if(!Staging) return false;

    void* Mapped = nullptr;
    const D3D12_RANGE NoRead{ 0, 0 };
    if(FAILED(Staging->Map(0, &NoRead, &Mapped))) return false;
    std::memcpy(Mapped, Data, size_t(Size));
    Staging->Unmap(0, nullptr);

    TransientCmd Cmd;
    if(!BeginTransientCmd(Device, Cmd)) return false;
    Cmd.List->CopyBufferRegion(Dest, 0, Staging.Get(), 0, Size);
    EndAndSubmitTransientCmd(Device, Cmd);
    return true;
}

/** One subresource's rows, tightly packed: RGBA8 texels, or a row of 4x4 blocks a row. */
struct SubresourceData
{
    const uint8_t* Bytes = nullptr;
    size_t         Size  = 0;
};

/**
 * Uploads InLevels into subresources 0..InCount-1 (mip 0 first), through one
 * staging range laid out by GetCopyableFootprints: each level's rows (texel
 * rows, or block rows for a BC format) as the footprint counts them, as many
 * as its bytes hold. Leaves the texture in PIXEL_SHADER_RESOURCE.
 *
 * InAsync: the texture is fresh (nothing has sampled it, so nothing in flight
 * can), so the upload is submitted without a wait, staged in the device's
 * ring when it has room. Null: a texture already in use, or no driver; staged
 * in a buffer of its own, and the device drained so the frames in flight
 * finish reading the old contents.
 */
inline bool UploadSubresources(D3D12Device* Device, ID3D12Resource* Texture, D3D12_RESOURCE_STATES& InOutState,
                               const SubresourceData* InLevels, uint32_t InCount, D3D12DynamicGDI* InAsync = nullptr)
{
    const D3D12_RESOURCE_DESC Desc = Texture->GetDesc();
    if(InCount == 0 || InCount > Desc.MipLevels) return false;

    std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> Footprints(InCount);
    std::vector<UINT>                               RowCounts(InCount);
    std::vector<UINT64>                             RowSizes(InCount);
    UINT64 TotalBytes = 0;
    Device->GetDevice()->GetCopyableFootprints(&Desc, 0, InCount, 0, Footprints.data(), RowCounts.data(), RowSizes.data(), &TotalBytes);

    /* A ring range starts 512-aligned (D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT),
     * so the footprints, moved by its offset, stay aligned. */
    ID3D12Resource*                        StagingBuffer = nullptr;
    Microsoft::WRL::ComPtr<ID3D12Resource> OwnStaging;
    UINT64                                 RingOffset    = 0;
    uint8_t*                               Mapped        = nullptr;
    const bool bRing = InAsync && Device->GetStagingRing().Allocate(TotalBytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT,
                                                                    InAsync->GetRecordingFenceValue(),
                                                                    StagingBuffer, RingOffset, Mapped);
    if(bRing)
    {
        for(D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint : Footprints) { Footprint.Offset += RingOffset; }
        Mapped -= RingOffset;   // the buffer's start: indexed by the footprints' offsets below
    }
    else
    {
        OwnStaging = CreateBuffer(Device, TotalBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        if(!OwnStaging) return false;
        void* Address = nullptr;
        const D3D12_RANGE NoRead{ 0, 0 };
        if(FAILED(OwnStaging->Map(0, &NoRead, &Address))) return false;
        Mapped        = static_cast<uint8_t*>(Address);
        StagingBuffer = OwnStaging.Get();
    }

    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& Footprint = Footprints[Level];
        const SubresourceData& Source = InLevels[Level];
        const size_t RowBytes = size_t(RowSizes[Level]);
        for(uint32_t Row = 0; Row < RowCounts[Level] && (Row + 1) * RowBytes <= Source.Size; ++Row)
        {
            std::memcpy(Mapped + Footprint.Offset + Row * Footprint.Footprint.RowPitch,
                        Source.Bytes + Row * RowBytes, RowBytes);
        }
    }
    if(OwnStaging) OwnStaging->Unmap(0, nullptr);

    TransientCmd Cmd;
    if(!BeginTransientCmd(Device, Cmd)) return false;

    RecordTransition(Cmd.List.Get(), Texture, InOutState, D3D12_RESOURCE_STATE_COPY_DEST);

    for(uint32_t Level = 0; Level < InCount; ++Level)
    {
        D3D12_TEXTURE_COPY_LOCATION Dst{};
        Dst.pResource        = Texture;
        Dst.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        Dst.SubresourceIndex = Level;

        D3D12_TEXTURE_COPY_LOCATION Src{};
        Src.pResource       = StagingBuffer;
        Src.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        Src.PlacedFootprint = Footprints[Level];

        Cmd.List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, nullptr);
    }

    RecordTransition(Cmd.List.Get(), Texture, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    EndAndSubmitTransientCmd(Device, Cmd, InAsync, std::move(OwnStaging));

    InOutState = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    return true;
}

/** Uploads tightly-packed RGBA8 rows into subresource 0. Leaves the texture in PIXEL_SHADER_RESOURCE. */
inline bool UploadTexture(D3D12Device* Device, ID3D12Resource* Texture, D3D12_RESOURCE_STATES& InOutState,
                          const uint8_t* Pixels, uint32_t Width, uint32_t Height, D3D12DynamicGDI* InAsync = nullptr)
{
    const SubresourceData Level{ Pixels, size_t(Width) * Height * 4u };
    return UploadSubresources(Device, Texture, InOutState, &Level, 1, InAsync);
}

/** Reads a Width x Height region at (X, Y) into OutPixels, InBytesPerTexel each (4: RGBA8; 8: RGBA16F),
 *  tightly packed. The texture is returned to InState. */
inline bool ReadbackTexture(D3D12Device* Device, ID3D12Resource* Texture, D3D12_RESOURCE_STATES InState,
                            uint32_t X, uint32_t Y, uint32_t Width, uint32_t Height,
                            std::vector<uint8_t>& OutPixels, uint32_t InBytesPerTexel = 4)
{
    D3D12_RESOURCE_DESC RegionDesc = Texture->GetDesc();
    RegionDesc.Width  = Width;
    RegionDesc.Height = Height;

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT Footprint{};
    UINT64 TotalBytes = 0;
    Device->GetDevice()->GetCopyableFootprints(&RegionDesc, 0, 1, 0, &Footprint, nullptr, nullptr, &TotalBytes);

    Microsoft::WRL::ComPtr<ID3D12Resource> Readback =
        CreateBuffer(Device, TotalBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    if(!Readback) return false;

    TransientCmd Cmd;
    if(!BeginTransientCmd(Device, Cmd)) return false;

    RecordTransition(Cmd.List.Get(), Texture, InState, D3D12_RESOURCE_STATE_COPY_SOURCE);

    D3D12_TEXTURE_COPY_LOCATION Dst{};
    Dst.pResource       = Readback.Get();
    Dst.Type            = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    Dst.PlacedFootprint = Footprint;

    D3D12_TEXTURE_COPY_LOCATION Src{};
    Src.pResource        = Texture;
    Src.Type             = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    Src.SubresourceIndex = 0;

    const D3D12_BOX Box{ X, Y, 0, X + Width, Y + Height, 1 };
    Cmd.List->CopyTextureRegion(&Dst, 0, 0, 0, &Src, &Box);

    RecordTransition(Cmd.List.Get(), Texture, D3D12_RESOURCE_STATE_COPY_SOURCE, InState);
    EndAndSubmitTransientCmd(Device, Cmd);

    void* Mapped = nullptr;
    if(FAILED(Readback->Map(0, nullptr, &Mapped))) return false;

    const size_t RowBytes = size_t(Width) * InBytesPerTexel;
    OutPixels.resize(RowBytes * Height);
    for(uint32_t Row = 0; Row < Height; ++Row)
    {
        std::memcpy(OutPixels.data() + Row * RowBytes,
                    static_cast<const uint8_t*>(Mapped) + Footprint.Offset + Row * Footprint.Footprint.RowPitch,
                    RowBytes);
    }

    const D3D12_RANGE NoWrite{ 0, 0 };
    Readback->Unmap(0, &NoWrite);
    return true;
}

} // namespace D3D12Ops

#endif // CORECLIENT_D3D12TRANSIENTOPS_H
