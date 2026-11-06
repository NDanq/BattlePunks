#ifndef CORECLIENT_NULLSYNC_H
#define CORECLIENT_NULLSYNC_H

#include "Public/GDI/GDISync.h"

/*
 * A fence that never needs waiting on. There is no GPU timeline behind
 * NullDynamicGDI, so every call to Fence()/WaitSync() is already "done" --
 * IsFenced() returns false so a caller never blocks expecting a signal that
 * would never arrive.
 */
class NullSync : public GDISync
{
public:
    NullSync() = default;

    void Initialize() override {}
    void Fence() override {}
    void WaitSync() override {}
    void DestroySync() override {}
    bool IsFenced() override { return false; }
};

#endif //CORECLIENT_NULLSYNC_H
