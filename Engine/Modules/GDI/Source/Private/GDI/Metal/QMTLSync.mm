#include "Public/GDI/Metal/QMTLSync.h"
#include "Public/GDI/Metal/QMTLDevice.h"

#include <dispatch/dispatch.h>

QMTLSync::QMTLSync() = default;

QMTLSync::~QMTLSync()
{
    DestroySync();
}

void QMTLSync::Bind(QMTLDevice* InDevice)
{
    if(Device == InDevice) return;
    DestroySync();
    Device = InDevice;
    Initialize();
}

void QMTLSync::Initialize()
{
    if(!Device || !Device->IsValid()) return;
    if(Event != nullptr)              return;

    Event = [Device->GetDevice() newSharedEvent];   // +1
    Event.label = @"Odessa.RenderTimeline";

    // A serial queue services the CPU-side wait callbacks for the event listener.
    dispatch_queue_t Queue = dispatch_queue_create("com.moonshotfragments.odessa.mtlsync",
                                                    DISPATCH_QUEUE_SERIAL);
    WaitQueue = (void*)Queue;
    MTLSharedEventListener* L = [[MTLSharedEventListener alloc] initWithDispatchQueue:Queue];
    Listener = (void*)L;

    SignaledTarget = 0;
    bFenced        = false;
}

uint64_t QMTLSync::AdvanceSignalValue()
{
    return ++SignaledTarget;
}

void QMTLSync::Fence()
{
    // The event is signaled by the EndRendering command buffer via encodeSignalEvent:;
    // mark fenced so the next WaitSync blocks.
    bFenced = true;
}

void QMTLSync::WaitSync()
{
    if(!Event || !bFenced) return;

    // Fast path: already reached.
    if(Event.signaledValue >= SignaledTarget)
    {
        bFenced = false;
        return;
    }

    MTLSharedEventListener* L = (MTLSharedEventListener*)Listener;
    if(L == nullptr)
    {
        bFenced = false;
        return;
    }

    dispatch_semaphore_t Sem = dispatch_semaphore_create(0);
    const uint64_t Target = SignaledTarget;
    [Event notifyListener:L
                  atValue:Target
                    block:^(id<MTLSharedEvent> /*e*/, uint64_t /*v*/)
    {
        dispatch_semaphore_signal(Sem);
    }];
    dispatch_semaphore_wait(Sem, DISPATCH_TIME_FOREVER);
    dispatch_release(Sem);   // MRC (-fno-objc-arc): explicit release of the dispatch object.

    bFenced = false;
}

void QMTLSync::DestroySync()
{
    if(Listener != nullptr)
    {
        [(MTLSharedEventListener*)Listener release];
        Listener = nullptr;
    }
    if(WaitQueue != nullptr)
    {
        dispatch_release((dispatch_queue_t)WaitQueue);
        WaitQueue = nullptr;
    }
    if(Event != nullptr)
    {
        [Event release];
        Event = nullptr;
    }
    SignaledTarget = 0;
    bFenced        = false;
}

bool QMTLSync::IsFenced()
{
    return bFenced;
}
