// VulkanCommon.glsl — macro definitions for the Vulkan / glslc code path.
// Included by Common.glsl when WITH_VULKAN is defined.
// -----------------------------------------------------------------------
// NOTE: binding-index parameters are named "slot" (not "binding") so the
// Python preprocessor does not accidentally replace the GLSL qualifier
// keyword "binding" that also appears in the macro body.

// --- Interface variables ------------------------------------------------
// Explicit location required by SPIR-V on every in/out variable.
// Callers write ATTRIB(...) with NO trailing semicolon.
#define ATTRIB(index, inout, type, name) layout(location=index) inout type name;

// Fragment colour output.
#define OUTPUT(idx, type, name) layout(location=idx) out type name;

// --- Texture samplers ---------------------------------------------------
// Explicit set + binding required by Vulkan.
#define SAMPLER2D(slot, name)      layout(set=0, binding=slot) uniform sampler2D name;
#define SAMPLER2D_RECT(slot, name) layout(set=0, binding=slot) uniform sampler2DRect name;

// --- Uniform / UBO declarations -----------------------------------------
// Single-struct UBO: emit a named block (set=0). Single line — no backslash continuation.
#define DECLARE_UBO(slot, BlockName, type, varname) layout(std140, set=0, binding=slot) uniform BlockName { type varname; };

// Multi-field per-shader block.
#define UBO_BEGIN(slot, BlockName) layout(std140, set=0, binding=slot) uniform BlockName {
#define SHADER_UNIFORM(type, name) type name;
#define UBO_END(BlockName)         };

// No-op: bare uniform variables are illegal in Vulkan GLSL.
// UBOs are declared via DECLARE_UBO or UBO_BEGIN/END.
#define UNIFORM(type, name)
