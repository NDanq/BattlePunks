#include "Public/GDI/Metal/MTLCommandManager.h"
#include "Public/GDI/Metal/MTLDevice.h"
#include "Public/GDI/GDILog.h"

MTLCommandManager::~MTLCommandManager()
{
    Shutdown();
}

bool MTLCommandManager::Initialize(MTLDevice* InDevice)
{
    Device = InDevice;
    if(!Device || !Device->IsValid())
    {
        GDI_LOG("[MTLCommandManager] Invalid device.")
        return false;
    }
    return true;
}

void MTLCommandManager::Shutdown()
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

MTLCommandBufferRef MTLCommandManager::BeginFrame()
{
    if(!Device || !Device->IsValid()) return nullptr;

    // Single frame in flight: block until the previous submission has completed
    // before recording the next one (the analog of Vulkan's FrameFence wait).
    WaitIdle();
    ReleaseLastCommitted();

    CommandBuffer = [[Device->GetGraphicsQueue() commandBuffer] retain];   // +1
    if(CommandBuffer == nullptr)
    {
        GDI_LOG("[MTLCommandManager] commandBuffer returned nil.")
        return nullptr;
    }
    CommandBuffer.label = @"Odessa.RenderFrame";
    bRecording = true;
    return CommandBuffer;
}

void MTLCommandManager::EndFrame()
{
    bRecording = false;
}

void MTLCommandManager::Commit()
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

void MTLCommandManager::WaitIdle()
{
    if(LastCommitted != nullptr)
    {
        [LastCommitted waitUntilCompleted];
    }
}

void MTLCommandManager::ReleaseLastCommitted()
{
    if(LastCommitted != nullptr)
    {
        [LastCommitted release];
        LastCommitted = nullptr;
    }
}
