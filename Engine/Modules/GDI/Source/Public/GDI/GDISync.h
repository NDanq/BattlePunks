#ifndef CORECLIENT_GDISYNC_H
#define CORECLIENT_GDISYNC_H

class GDISync
{
public:
    explicit GDISync() {  }
    virtual void Initialize() = 0;
    virtual void Fence() = 0;
    virtual void WaitSync() = 0;
    virtual void DestroySync() = 0;
    virtual bool IsFenced() = 0;
};

#endif //CORECLIENT_GDISYNC_H
