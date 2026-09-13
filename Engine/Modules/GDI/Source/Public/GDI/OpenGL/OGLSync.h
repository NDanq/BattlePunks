#ifndef CORECLIENT_OGLSYNC_H
#define CORECLIENT_OGLSYNC_H
#include "Public/GDI/OpenGL/GDIOpenGL.h"

#include "Public/GDI/GDISync.h"

class OGLSync : public GDISync, protected GDIGLFunctions
{
public:
    explicit OGLSync();
    void Initialize();
    void Fence() override;
    void WaitSync() override;
    void DestroySync() override;
    bool IsFenced() override;
protected:
    GLsync Sync{};
};

#endif //CORECLIENT_OGLSYNC_H
