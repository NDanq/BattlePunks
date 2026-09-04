#include "Public/GDI/OpenGL/GDIOpenGL.h"

#include "Public/GDI/GDILog.h"
#include "Public/GDI/GDIPlatform.h"

namespace GDIGLLoader
{
// Storage for every entry point, generated from the same list that declares
// them -- so a function cannot be declared and then forgotten by the loader.
#define GDI_GL_DEFINE_PTR(Ret, Name, Params, Args) PFN_##Name P_##Name = nullptr;
GDI_GL_FUNCTIONS(GDI_GL_DEFINE_PTR)
#undef GDI_GL_DEFINE_PTR

namespace
{
    bool GbLoaded = false;
}

bool IsLoaded()
{
    return GbLoaded;
}

bool LoadFunctions()
{
    // Idempotent: this stands in for initializeOpenGLFunctions(), which the
    // backends call from about a dozen constructors.
    if(GbLoaded)
    {
        return true;
    }

    const char* FirstMissing = nullptr;

    /*
     * Resolve every entry, recording the first failure rather than stopping.
     * Loading everything means a driver missing several functions reports the
     * first one and leaves the rest null-checked in one place, instead of
     * surfacing as an access violation on first draw.
     */
#define GDI_GL_LOAD_PTR(Ret, Name, Params, Args)                                            \
    P_##Name = reinterpret_cast<PFN_##Name>(GDIPlatform::appGLGetProcAddress(#Name)); \
    if(P_##Name == nullptr && FirstMissing == nullptr) { FirstMissing = #Name; }

    GDI_GL_FUNCTIONS(GDI_GL_LOAD_PTR)
#undef GDI_GL_LOAD_PTR

    if(FirstMissing != nullptr)
    {
        GDI_LOG_ERROR(std::string("[GDIGL] Failed to resolve OpenGL entry point '") + FirstMissing
                      + "'. Is a context current, and does it meet the 4.4 core requirement?")
        return false;
    }

    GbLoaded = true;
    GDI_LOG("[GDIGL] OpenGL entry points loaded.")
    return true;
}
}

bool GDIGLFunctions::hasOpenGLFramebufferBlit()
{
    /*
     * Replaces QOpenGLFramebufferObject::hasOpenGLFramebufferBlit(), which was
     * the only use of QOpenGLFramebufferObject anywhere in this backend.
     *
     * glBlitFramebuffer is core from GL 3.0 and this backend already requires
     * 4.4+, so "did it resolve" is both the honest test and the one that
     * actually matters -- a driver that gave us a context but no blit entry
     * point would fail here rather than at the call.
     */
    return GDIGLLoader::IsLoaded() && GDIGLLoader::P_glBlitFramebuffer != nullptr;
}
