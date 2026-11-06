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
// DECLARE_UBO is GONE from this file. It used to expand to a block wrapping
// ONE MEMBER OF STRUCT TYPE `type` -- `uniform BlockName { type varname; };`
// -- which compiles and runs correctly under glslc/Vulkan, but SPIR-V-Cross's
// HLSL backend cannot translate it for the D3D12 cook: a cbuffer member
// whose own type is a nested struct has no HLSL packing/packoffset
// expression, and spirv-cross throws ("cannot be expressed with either
// HLSL packing layout or packoffset") the moment it tries. HLSL cbuffers
// want their fields flattened directly into the buffer body; a nested
// struct is a GLSL-only idiom with no HLSL equivalent to lower to.
//
// A generic replacement macro was tried and abandoned: cook_shaders.py's
// preprocessor (Scripts/cook_shaders.py) is a small line-based one, not a
// real C preprocessor -- #define bodies are single physical lines (no
// backslash continuation) and function-like macro expansion is a plain
// comma-split with no token-pasting (no ##) and no awareness of nested
// parens, so a macro argument like a struct constructor call
// (`GDIViewParameters(Projection, View, MouseCoords, Resolution)`) would be
// mis-split at its own internal commas. DECLARE_UBO had exactly two callers
// in the whole shader tree (VP_Block/GDIViewParameters and
// TR_Block/GDITransform, both in Common.glsl), so rather than fight the
// preprocessor for a generic macro used twice, both are declared directly,
// by hand, in Common.glsl -- flat cbuffer fields, plus a local
// GDIViewParameters/GDITransform variable copied from them so every
// existing call site (Basic.vsh's `ViewParameters.Projection`, etc.) keeps
// working unchanged.

// Multi-field per-shader block.
#define UBO_BEGIN(slot, BlockName) layout(std140, set=0, binding=slot) uniform BlockName {
#define SHADER_UNIFORM(type, name) type name;
#define UBO_END(BlockName)         };

// No-op: bare uniform variables are illegal in Vulkan GLSL.
// UBOs are declared via DECLARE_UBO or UBO_BEGIN/END.
#define UNIFORM(type, name)
