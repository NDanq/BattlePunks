// OpenGLCommon.glsl — macro definitions for the OpenGL 4.4+ GLSL code path.
// Included by Common.glsl when WITH_OPENGL is defined.
// -----------------------------------------------------------------------

// --- Interface variables ------------------------------------------------
// Vertex / stage input or output with explicit location.
// Callers write ATTRIB(...) with NO trailing semicolon.
#define ATTRIB(index, inout, type, name) layout(location=index) inout type name;

// Fragment colour output at an explicit location.
#define OUTPUT(idx, type, name) layout(location=idx) out type name;

// --- Texture samplers ---------------------------------------------------
// binding is accepted but ignored by OpenGL's default uniform binding.
#define SAMPLER2D(binding, name)      uniform sampler2D name;
#define SAMPLER2D_RECT(binding, name) uniform sampler2DRect name;

// --- Uniform / UBO declarations -----------------------------------------
// Single-struct UBO: on OpenGL expand to a loose uniform (C++ uses glUniform*).
// binding, BlockName are accepted but unused.
#define DECLARE_UBO(binding, BlockName, type, varname) uniform type varname;

// Multi-field per-shader block: collapse to individual loose uniforms.
#define UBO_BEGIN(binding, BlockName)
#define SHADER_UNIFORM(type, name)    uniform type name;
#define UBO_END(BlockName)

// Legacy compatibility — kept for any remaining callers.
#define UNIFORM(type, name) uniform type name;
