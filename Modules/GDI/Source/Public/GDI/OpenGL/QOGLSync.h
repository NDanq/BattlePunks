#ifndef CORECLIENT_QOGLSYNC_H
#define CORECLIENT_QOGLSYNC_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"

#include "Public/GDI/GDISync.h"

class QOGLSync : public GDISync, protected GDIGLFunctions
{
public:
    explicit QOGLSync();
    void Initialize();
    void Fence() override;
    void WaitSync() override;
    void DestroySync() override;
    bool IsFenced() override;
protected:
    GLsync Sync{};
};

#endif //CORECLIENT_QOGLSYNC_H
