#ifndef CORECLIENT_D3D12RECORDINGCONTEXT_H
#define CORECLIENT_D3D12RECORDINGCONTEXT_H

#include <d3d12.h>
#include <wrl/client.h>

#include "Public/GDI/D3D12/D3D12DynamicGDI.h"

/**
 * A parallel pass's recording context on D3D12 (GDIRecordingContext; RenderCore's
 * MULTITHREADED_RENDERING.md, phase 2): one thread's chunk of the pass, recorded
 * into its own direct command list.
 *
 * Everything a draw writes is its own -- the list, the block uploads' cache,
 * the blend and depth states, the core blocks -- so contexts record at once.
 * What they share is read-only while the pass is open (the device, the root
 * signature, the shaders, meshes and textures), locked (the pipeline cache), or
 * claimed atomically (the constant-buffer ring).
 *
 * Kept by the driver across frames (D3D12DynamicGDI::RecordingContexts). Each
 * pass reopens the list on the context's allocator, which keeps every pass's
 * commands until BeginRendering rewinds it, after the frame fence's wait.
 */
class D3D12RecordingContext : public GDIRecordingContext
{
public:
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator>    Allocator;
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> List;
    D3D12BlockUploads                                 Uploads;
    IBlendStateDesc                                   Blend;
    EColorMask                                        ColorMask = EColM_RGBA;
    IDepthStateDesc                                   Depth;
    float                                             CoreView[40]      = {};
    float                                             CoreTransform[36] = {};
    bool                                              bCoreView      = false;
    bool                                              bCoreTransform = false;
    int                                               EventDepth     = 0;
    bool                                              bRecording     = false;
};

#endif //CORECLIENT_D3D12RECORDINGCONTEXT_H
