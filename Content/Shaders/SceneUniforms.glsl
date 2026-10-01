// SceneUniforms.glsl -- the scene block (binding 3, GDI_SCENE_BLOCK_BINDING),
// filled once a frame by RenderCore's RenderContext. Its C++ mirror is
// RenderCore::SceneUniforms (Private/RenderCore/MaterialRuntime.h), whose
// static_asserts pin these offsets: every member is a vec4, so std140 adds
// no padding.

UBO_BLOCK_BEGIN(3, Scene_Block)
    vec4 Scene_CameraPosition;   // xyz: the eye, world space
    vec4 Scene_Time;             // x: seconds since the renderer started; y: the last frame's length
    vec4 Scene_LightDirection;   // xyz: unit vector toward the light; w: intensity
    vec4 Scene_LightColor;       // rgb, display space
    vec4 Scene_SkyColor;         // rgb: ambient from above (-Y); a: ambient intensity
    vec4 Scene_GroundColor;      // rgb: ambient from below (+Y)
UBO_BLOCK_END(Scene_Block)
