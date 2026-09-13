#include "Public/GDI/OpenGL/OGLSync.h"
#include "Public/GDI/GDILog.h"

OGLSync::OGLSync() : GDISync()
{

}

void OGLSync::Initialize()
{
    initializeOpenGLFunctions();
}

void OGLSync::Fence()
{
    if(Sync == NULL)
    {
        Sync = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    }
}

void OGLSync::WaitSync()
{
    if(Sync == NULL) return;
    while(GLenum WaitStatus = glClientWaitSync(Sync, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL))
    {
        switch(WaitStatus)
        {
            case GL_ALREADY_SIGNALED:
                continue;
            case GL_TIMEOUT_EXPIRED:
                GDI_CHECK(0, "Timeout Expired")
                break;
            case GL_CONDITION_SATISFIED:
                DestroySync();
                return;
            case GL_WAIT_FAILED:
                GLenum Error = glGetError();
                if(Error == GL_INVALID_ENUM || Error == GL_INVALID_VALUE)
                {
                    GDI_CHECK(0, "Wait Failed")
                }
                break;
        }
    }
}

bool OGLSync::IsFenced()
{
    return Sync == NULL;
}

void OGLSync::DestroySync()
{
    glDeleteSync(Sync);
    Sync = NULL;
}
