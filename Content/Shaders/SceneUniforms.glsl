// SceneUniforms.glsl -- the scene block (binding 3, GDI_SCENE_BLOCK_BINDING),
// filled once a frame by RenderCore's RenderContext. Its C++ mirror is
// SceneUniforms in RenderContext.cpp, whose static_asserts pin these offsets:
// every member is a vec4 or a mat4, so std140 adds no padding.

UBO_BLOCK_BEGIN(3, Scene_Block)
    vec4 Scene_CameraPosition;   // xyz: the eye, world space
    vec4 Scene_Time;             // x: seconds since the renderer started; y: the last frame's length
    vec4 Scene_LightDirection;   // xyz: unit vector toward the light; w: intensity
    vec4 Scene_LightColor;       // rgb, display space
    vec4 Scene_SkyColor;         // rgb: ambient from above (-Y); a: ambient intensity
    vec4 Scene_GroundColor;      // rgb: ambient from below (+Y)
    mat4 Scene_InvViewProjection; // clip space (GL's: z_ndc = 2 * depth - 1) back to world space
UBO_BLOCK_END(Scene_Block)
