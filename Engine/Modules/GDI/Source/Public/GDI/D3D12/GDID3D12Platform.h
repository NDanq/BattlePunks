#ifndef CORECLIENT_GDID3D12PLATFORM_H
#define CORECLIENT_GDID3D12PLATFORM_H

#include <dxgi1_2.h>

#include "Public/Core/CoreTypes.h"

/*
 * The D3D12 half of GDI's window-system seam, implemented in
 * Source/Private/GDI/Platform/. The window handle is the only thing the
 * backend needs from the platform, and this is the only place it is used.
 */

namespace GDIPlatform
{
    HRESULT appD3D12CreateSwapChain(IDXGIFactory2* InFactory,
                                    IUnknown* InCommandQueue,
                                    EngineCore::WindowHandle InWindow,
                                    const DXGI_SWAP_CHAIN_DESC1& InDesc,
                                    IDXGISwapChain1** OutSwapChain);
}

#endif //CORECLIENT_GDID3D12PLATFORM_H
