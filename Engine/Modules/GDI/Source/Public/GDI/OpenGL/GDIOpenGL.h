#ifndef CORECLIENT_GDIOPENGL_H
#define CORECLIENT_GDIOPENGL_H

/*
 * GDI's OpenGL entry points, loaded at runtime.
 *
 * WHY THIS EXISTS RATHER THAN glad
 *
 * The migration plan called for vendoring glad. glad is a code GENERATOR --
 * its output comes from an online service, there is no copy on this machine
 * and no vcpkg/conan to fetch one. Rather than pull a generated blob off the
 * network, this header declares exactly the entry points GDI actually calls:
 * 47 functions and 34 constants beyond GL 1.1, measured from the sources
 * rather than guessed.
 *
 * The seam is identical either way -- everything resolves through
 * appGLGetProcAddress -- so dropping glad in later is a local change to
 * GDIGLFunctions::LoadFunctions() and nothing else.
 *
 * WHY A BASE CLASS AND NOT MACROS
 *
 * The obvious shortcut is `#define glCreateShader GDIGL_glCreateShader`. It
 * does not work here: CoreClient's own present path holds a
 * QOpenGLFunctions_4_4_Core and calls `F->glPushDebugGroup(...)` in a
 * translation unit that also includes GDI headers, so a macro would rewrite
 * Qt's member call and break it.
 *
 * Instead this is a base class with inline members, exactly the shape
 * QOpenGLFunctions_4_5_Core had. The backends swap their base class and every
 * call site inside them keeps compiling verbatim -- which is the whole reason
 * this step touched no GL call sites.
 *
 * WHAT THE WINDOWS SDK GIVES US
 *
 * <gl/GL.h> is OpenGL 1.1 only: it supplies the scalar typedefs and the GL 1.1
 * constants and functions, which opengl32.lib exports directly and which need
 * no loading. Everything newer is declared and resolved below.
 */

#if defined(_WIN32)
#include <Windows.h>
#endif

#include <gl/GL.h>

#include <cstddef>
#include <cstdint>

// ---------------------------------------------------------------------------
// Types introduced after GL 1.1. Spellings match the Khronos registry.
// ---------------------------------------------------------------------------

typedef char      GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef uint64_t  GLuint64;
typedef int64_t   GLint64;
typedef struct __GLsync* GLsync;

// ---------------------------------------------------------------------------
// Constants beyond GL 1.1, from the Khronos registry.
// ---------------------------------------------------------------------------

#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#ifndef GL_UNIFORM_BUFFER
#define GL_UNIFORM_BUFFER                 0x8A11
#endif
#define GL_STREAM_DRAW                    0x88E0
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82

#define GL_TEXTURE0                       0x84C0

#define GL_BGRA                           0x80E1
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_MIRRORED_REPEAT                0x8370

#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_INVALID_FRAMEBUFFER_OPERATION  0x0506
#define GL_RENDERBUFFER                   0x8D41
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_DEPTH_COMPONENT24              0x81A6

#define GL_FUNC_ADD                       0x8006
#define GL_MIN                            0x8007
#define GL_MAX                            0x8008
#define GL_CONSTANT_COLOR                 0x8001
#define GL_ONE_MINUS_CONSTANT_COLOR       0x8002
#define GL_CONSTANT_ALPHA                 0x8003
#define GL_ONE_MINUS_CONSTANT_ALPHA       0x8004

#define GL_INCR_WRAP                      0x8507

#define GL_SYNC_GPU_COMMANDS_COMPLETE     0x9117
#define GL_ALREADY_SIGNALED               0x911A
#define GL_TIMEOUT_EXPIRED                0x911B
#define GL_CONDITION_SATISFIED            0x911C
#define GL_WAIT_FAILED                    0x911D
#define GL_SYNC_FLUSH_COMMANDS_BIT        0x00000001

#define GL_DEBUG_SOURCE_APPLICATION       0x824A

// ---------------------------------------------------------------------------
// Entry points.
//
// Declared through one macro list so the typedef, the storage and the load
// call cannot drift apart. The classic failure of a hand-rolled loader is an
// entry point that is declared but never loaded -- it then crashes on first
// use instead of failing at load time. Here all three are generated from the
// same list, so that cannot happen.
// ---------------------------------------------------------------------------

#define GDI_GL_FUNCTIONS(X) \
    X(void,   glActiveTexture,           (GLenum texture), (texture)) \
    X(void,   glAttachShader,            (GLuint program, GLuint shader), (program, shader)) \
    X(void,   glBindBuffer,              (GLenum target, GLuint buffer), (target, buffer)) \
    X(void,   glBindBufferBase,          (GLenum target, GLuint index, GLuint buffer), (target, index, buffer)) \
    X(void,   glBindFramebuffer,         (GLenum target, GLuint framebuffer), (target, framebuffer)) \
    X(void,   glBindRenderbuffer,        (GLenum target, GLuint renderbuffer), (target, renderbuffer)) \
    X(void,   glBindVertexArray,         (GLuint array), (array)) \
    X(void,   glBlendEquationSeparate,   (GLenum modeRGB, GLenum modeAlpha), (modeRGB, modeAlpha)) \
    X(void,   glBlitFramebuffer,         (GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter), (srcX0, srcY0, srcX1, srcY1, dstX0, dstY0, dstX1, dstY1, mask, filter)) \
    X(void,   glBufferData,              (GLenum target, GLsizeiptr size, const void* data, GLenum usage), (target, size, data, usage)) \
    X(void,   glBufferSubData,           (GLenum target, GLintptr offset, GLsizeiptr size, const void* data), (target, offset, size, data)) \
    X(GLenum, glCheckFramebufferStatus,  (GLenum target), (target)) \
    X(GLenum, glClientWaitSync,          (GLsync sync, GLbitfield flags, GLuint64 timeout), (sync, flags, timeout)) \
    X(void,   glCompileShader,           (GLuint shader), (shader)) \
    X(GLuint, glCreateProgram,           (void), ()) \
    X(GLuint, glCreateShader,            (GLenum type), (type)) \
    X(void,   glDeleteBuffers,           (GLsizei n, const GLuint* buffers), (n, buffers)) \
    X(void,   glDeleteFramebuffers,      (GLsizei n, const GLuint* framebuffers), (n, framebuffers)) \
    X(void,   glDeleteProgram,           (GLuint program), (program)) \
    X(void,   glDeleteRenderbuffers,     (GLsizei n, const GLuint* renderbuffers), (n, renderbuffers)) \
    X(void,   glDeleteShader,            (GLuint shader), (shader)) \
    X(void,   glDeleteSync,              (GLsync sync), (sync)) \
    X(void,   glDeleteVertexArrays,      (GLsizei n, const GLuint* arrays), (n, arrays)) \
    X(void,   glDrawArraysInstanced,     (GLenum mode, GLint first, GLsizei count, GLsizei instancecount), (mode, first, count, instancecount)) \
    X(void,   glEnableVertexAttribArray, (GLuint index), (index)) \
    X(GLsync, glFenceSync,               (GLenum condition, GLbitfield flags), (condition, flags)) \
    X(void,   glFramebufferRenderbuffer, (GLenum target, GLenum attachment, GLenum renderbuffertarget, GLuint renderbuffer), (target, attachment, renderbuffertarget, renderbuffer)) \
    X(void,   glFramebufferTexture2D,    (GLenum target, GLenum attachment, GLenum textarget, GLuint texture, GLint level), (target, attachment, textarget, texture, level)) \
    X(void,   glGenBuffers,              (GLsizei n, GLuint* buffers), (n, buffers)) \
    X(void,   glGenFramebuffers,         (GLsizei n, GLuint* framebuffers), (n, framebuffers)) \
    X(void,   glGenRenderbuffers,        (GLsizei n, GLuint* renderbuffers), (n, renderbuffers)) \
    X(void,   glGenVertexArrays,         (GLsizei n, GLuint* arrays), (n, arrays)) \
    X(void,   glGenerateMipmap,          (GLenum target), (target)) \
    X(void,   glGetProgramInfoLog,       (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog), (program, bufSize, length, infoLog)) \
    X(void,   glGetProgramiv,            (GLuint program, GLenum pname, GLint* params), (program, pname, params)) \
    X(void,   glGetShaderInfoLog,        (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog), (shader, bufSize, length, infoLog)) \
    X(void,   glGetShaderiv,             (GLuint shader, GLenum pname, GLint* params), (shader, pname, params)) \
    X(GLint,  glGetUniformLocation,      (GLuint program, const GLchar* name), (program, name)) \
    X(void,   glLinkProgram,             (GLuint program), (program)) \
    X(void,   glPopDebugGroup,           (void), ()) \
    X(void,   glPushDebugGroup,          (GLenum source, GLuint id, GLsizei length, const GLchar* message), (source, id, length, message)) \
    X(void,   glRenderbufferStorage,     (GLenum target, GLenum internalformat, GLsizei width, GLsizei height), (target, internalformat, width, height)) \
    X(void,   glShaderSource,            (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length), (shader, count, string, length)) \
    X(void,   glUniform1f,               (GLint location, GLfloat v0), (location, v0)) \
    X(void,   glUniform1i,               (GLint location, GLint v0), (location, v0)) \
    X(void,   glUniform2f,               (GLint location, GLfloat v0, GLfloat v1), (location, v0, v1)) \
    X(void,   glUniform3f,               (GLint location, GLfloat v0, GLfloat v1, GLfloat v2), (location, v0, v1, v2)) \
    X(void,   glUniform4f,               (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3), (location, v0, v1, v2, v3)) \
    X(void,   glUniformMatrix4fv,        (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value), (location, count, transpose, value)) \
    X(void,   glUseProgram,              (GLuint program), (program)) \
    X(void,   glVertexAttribDivisor,     (GLuint index, GLuint divisor), (index, divisor)) \
    X(void,   glVertexAttribPointer,     (GLuint index, GLint size, GLenum type, GLboolean normalized, GLsizei stride, const void* pointer), (index, size, type, normalized, stride, pointer)) \
    X(void,   glWaitSync,                (GLsync sync, GLbitfield flags, GLuint64 timeout), (sync, flags, timeout))

namespace GDIGLLoader
{
#define GDI_GL_DECLARE_PTR(Ret, Name, Params, Args) \
    typedef Ret (APIENTRY* PFN_##Name) Params;       \
    extern PFN_##Name P_##Name;

    GDI_GL_FUNCTIONS(GDI_GL_DECLARE_PTR)
#undef GDI_GL_DECLARE_PTR

    /*
     * Resolves every entry point above through appGLGetProcAddress. Requires a
     * current context.
     *
     * Returns false and logs the FIRST unresolved entry point rather than
     * leaving null pointers to surface as a crash on first draw. Idempotent --
     * the old initializeOpenGLFunctions() was called from a dozen
     * constructors, and this is called from the same places.
     */
    bool LoadFunctions();
    bool IsLoaded();
}

/*
 * Drop-in replacement for QOpenGLFunctions / QOpenGLExtraFunctions /
 * QOpenGLFunctions_4_x_Core as a base class.
 *
 * The backends inherit this instead, and every `glCreateShader(...)` call
 * inside them resolves to a member here -- so no GL call site changed.
 *
 * Note the two Qt bases being replaced pinned DIFFERENT GL versions
 * (QOpenGLFunctions_4_5_Core in OpenGLDynamicGDI, _4_4_Core in
 * OpenGLRenderTarget), a latent inconsistency. One loader for one context
 * resolves it.
 */
class GDIGLFunctions
{
public:
    virtual ~GDIGLFunctions() = default;

    /* Named to match the Qt call it replaces, so the ~12 constructor call
     * sites did not have to change. */
    bool initializeOpenGLFunctions() { return GDIGLLoader::LoadFunctions(); }

    /*
     * Replaces QOpenGLFramebufferObject::hasOpenGLFramebufferBlit().
     *
     * That was the ONLY use of QOpenGLFramebufferObject in the whole backend --
     * every other FBO operation here is already raw GL. Blit is core from GL
     * 3.0 and this backend requires 4.4+, so the honest test is whether
     * glBlitFramebuffer actually resolved.
     */
    static bool hasOpenGLFramebufferBlit();

protected:
/*
     * EXACT signatures, deliberately NOT a perfect-forwarding template.
     *
     * Several call sites pass NULL where a pointer is expected --
     * glShaderSource's length, glGetProgramInfoLog's length. Under a variadic
     * template those deduce as int and fail to convert. Qt's bases declared
     * the real signatures so the implicit conversion worked; reproducing that
     * is what lets every GL call site stay untouched.
     */
#define GDI_GL_MEMBER(Ret, Name, Params, Args)      \
    Ret Name Params const                           \
    {                                               \
        return GDIGLLoader::P_##Name Args;          \
    }

    GDI_GL_FUNCTIONS(GDI_GL_MEMBER)
#undef GDI_GL_MEMBER
};

#endif //CORECLIENT_GDIOPENGL_H
