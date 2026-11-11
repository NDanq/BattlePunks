#ifndef CORECLIENT_GDISYNC_H
#define CORECLIENT_GDISYNC_H

class GDISync
{
public:
    explicit GDISync() {  }
    /* Virtual: GSync is a backend's subclass, deleted through this base. */
    virtual ~GDISync() = default;
    virtual void Initialize() = 0;
    virtual void Fence() = 0;
    virtual void WaitSync() = 0;
    virtual void DestroySync() = 0;
    virtual bool IsFenced() = 0;
};

#endif //CORECLIENT_GDISYNC_H
