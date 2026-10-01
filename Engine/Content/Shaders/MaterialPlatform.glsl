// MaterialPlatform.glsl -- per-platform helpers for material shaders beyond
// Common.glsl. Included first, only when used. Lives beside it in
// Engine/Content/Shaders, the include root of every cook.
//
// --- Control-flow hints -------------------------------------------------------
// Branch / For Loop hints, via GL_EXT_control_flow_attributes on SPIR-V (and
// on to D3D12 via spirv-cross). On OpenGL each is a comment: cook_shaders.py
// only substitutes defines that have a value.

#if WITH_VULKAN
#extension GL_EXT_control_flow_attributes : require
#define HINT_BRANCH  [[branch]]
#define HINT_FLATTEN [[flatten]]
#define HINT_LOOP    [[dont_unroll]]
#define HINT_UNROLL  [[unroll]]
#else
#define HINT_BRANCH  /* branch */
#define HINT_FLATTEN /* flatten */
#define HINT_LOOP    /* loop */
#define HINT_UNROLL  /* unroll */
#endif
