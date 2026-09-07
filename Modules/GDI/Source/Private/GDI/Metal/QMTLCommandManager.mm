#include "Public/GDI/Metal/QMTLCommandManager.h"
#include "Public/GDI/Metal/QMTLDevice.h"
#include "Public/GDI/GDILog.h"

QMTLCommandManager::~QMTLCommandManager()
{
    Shutdown();
}

bool QMTLCommandManager::Initialize(QMTLDevice* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid())
    {
        GDI_LOG("[QMTLCommandManager] Invalid device.")
        return false;
    }
    return true;
}

void QMTLCommandManager::Shutdown()
{
    WaitIdle();
    ReleaseLastCommitted();

    if(CommandBuffer != nullptr)
    {
        // An uncommitted buffer at shutdown: drop it without committing.
        [CommandBuffer release];
        CommandBuffer = nullptr;
    }
    Device     = nullptr;
    bRecording = false;
}

MTLCommandBufferRef QMTLCommandManager::BeginFrame()
{
    if(!Device || !Device->IsValid()) return nullptr;

    // Single frame in flight: block until the previous submission has completed
    // before recording the next one (the analog of Vulkan's FrameFence wait).
    WaitIdle();
    ReleaseLastCommitted();

    CommandBuffer = [[Device->GetGraphicsQueue() commandBuffer] retain];   // +1
    if(CommandBuffer == nullptr)
    {
        GDI_LOG("[QMTLCommandManager] commandBuffer returned nil.")
        return nullptr;
    }
    CommandBuffer.label = @"Odessa.RenderFrame";
    bRecording = true;
    return CommandBuffer;
}

void QMTLCommandManager::EndFrame()
{
    bRecording = false;
}

void QMTLCommandManager::Commit()
{
    if(CommandBuffer == nullptr) return;

    [CommandBuffer commit];

    // Hand the +1 from BeginFrame straight to LastCommitted -- no extra retain,
    // no release: ownership simply moves.
    ReleaseLastCommitted();
    LastCommitted = CommandBuffer;
    CommandBuffer = nullptr;
    bRecording    = false;
}

void QMTLCommandManager::WaitIdle()
{
    if(LastCommitted != nullptr)
    {
        [LastCommitted waitUntilCompleted];
    }
}

void QMTLCommandManager::ReleaseLastCommitted()
{
    if(LastCommitted != nullptr)
    {
        [LastCommitted release];
        LastCommitted = nullptr;
    }
}
