#include "Public/GDI/Metal/QMTLDevice.h"
#include "Public/GDI/GDILog.h"
#include <string>

// Qt types used by this file. These were arriving implicitly through
// Public/Odessa.h until GDI stopped including application headers;
// they are explicit now, and each one disappears as its type converts.

QMTLDevice::QMTLDevice() = default;

QMTLDevice::~QMTLDevice()
{
    Shutdown();
}

bool QMTLDevice::Initialize()
{
    if(Device != nullptr) return true;

    // MTLCreateSystemDefaultDevice() returns a +1 (retained) object.
    Device = MTLCreateSystemDefaultDevice();
    if(Device == nullptr)
    {
        GDI_LOG("[QMTLDevice] MTLCreateSystemDefaultDevice() returned nil -- no Metal GPU.")
        return false;
    }

    GraphicsQueue = [Device newCommandQueue];   // +1
    if(GraphicsQueue == nullptr)
    {
        GDI_LOG("[QMTLDevice] Failed to create the primary graphics command queue.")
        [Device release];
        Device = nullptr;
        return false;
    }
    GraphicsQueue.label = @"Odessa.GraphicsQueue";

    GDI_LOG("[QMTLDevice] Initialized Metal device: " + ::GDILogDetail::ToLogString([[Device name] UTF8String]))
    return true;
}

void QMTLDevice::Shutdown()
{
    if(GraphicsQueue != nullptr)
    {
        [GraphicsQueue release];
        GraphicsQueue = nullptr;
    }
    if(Device != nullptr)
    {
        [Device release];
        Device = nullptr;
    }
}

MTLCommandQueueRef QMTLDevice::CreateCommandQueue() const
{
    if(Device == nullptr) return nullptr;
    return [Device newCommandQueue];    // +1; caller owns.
}
