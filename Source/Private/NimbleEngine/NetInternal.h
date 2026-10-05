#ifndef NIMBLEENGINE_NETINTERNAL_H
#define NIMBLEENGINE_NETINTERNAL_H

/* Between NetHost.cpp, which knows how the session was started, and
 * NetFacade.cpp, which answers games. Not exported. */
namespace NimbleEngine
{
    void SetDedicatedServerFlag(bool bInDedicated);
}

#endif //NIMBLEENGINE_NETINTERNAL_H
