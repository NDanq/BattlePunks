// MaterialPlatform.glsl -- what a material graph's shader needs from each
// platform beyond GDI's Common.glsl. A generated .fsh includes it first, before
// Common.glsl, when it uses anything here. Resolved by
// Scripts/cook_material_graphs.py, which looks in this folder after GDI's
// Shaders/ (GDI is vendored and never edited from NodeGraph).
//
// --- Control-flow hints -------------------------------------------------------
// A Branch or For Loop node's Hint. SPIR-V carries them
// (GL_EXT_control_flow_attributes): to Vulkan, and through spirv-cross to
// D3D12 as [branch], [flatten], [loop] and [unroll]. OpenGL drivers do not take
// the attributes, so there each is only a comment -- a no-op. A comment, not
// nothing: cook_shaders.py's preprocessor substitutes only defines with a value.

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
