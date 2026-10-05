#ifndef NIMBLEENGINE_NETINTERNAL_H
#define NIMBLEENGINE_NETINTERNAL_H

#include "Public/NimbleEngine/NetFacade.h"

/* Between NetHost.cpp, which knows how the session was started, and
 * NetFacade.cpp, which answers games. Not exported. */
namespace NimbleEngine
{
    void SetDedicatedServerFlag(bool bInDedicated);

    /* The game's prediction handler (NetFacade.h), for NetHost to call. */
    NetCorrectionFn GetNetCorrectionHandler(void*& OutUserData);
}

#endif //NIMBLEENGINE_NETINTERNAL_H
