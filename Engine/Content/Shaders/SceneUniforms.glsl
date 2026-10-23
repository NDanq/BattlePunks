// SceneUniforms.glsl -- the scene block (binding 3, GDI_SCENE_BLOCK_BINDING),
// filled once a frame by RenderCore's RenderContext. Its C++ mirror is
// SceneUniforms in RenderContext.cpp, whose static_asserts pin these offsets:
// every member is a vec4, a mat4 or an array of them, so std140 adds no
// padding.

UBO_BLOCK_BEGIN(3, Scene_Block)
    vec4 Scene_CameraPosition;   // xyz: the eye, world space
    vec4 Scene_Time;             // x: seconds since the renderer started; y: the last frame's length
    vec4 Scene_LightDirection;   // xyz: unit vector toward the light; w: intensity
    vec4 Scene_LightColor;       // rgb, display space
    vec4 Scene_SkyColor;         // rgb: ambient from above (+Y); a: ambient intensity
    vec4 Scene_GroundColor;      // rgb: ambient from below (-Y)
    mat4 Scene_InvViewProjection; // clip space (GL's: z_ndc = 2 * depth - 1) back to world space

    // Shadows (Shadows.glsl, RENDERPIPELINE.md's "Shadows").
    vec4 Scene_ShadowParams;     // x: 1 if any shadow is drawn; y: PCF radius in texels (0-2); z: forward draws' (0-2); w: atlas size
    vec4 Scene_LightShadow;      // the sun's: x technique (-1 none, 0 Standard, 1 Modulated, 2 Cascaded); y cascades; z depth bias; w normal bias
    vec4 Scene_LightShadowTint;  // rgb: the sun's Modulated tint, display space
    vec4 Scene_CascadeSplits;    // each cascade's far edge, as view depth
    vec4 Scene_CameraForward;    // xyz: the view's forward; w: dot(its origin, forward) -- view depth = dot(P, xyz) - w
    mat4 Scene_CascadeMatrices[4]; // world to (u, v, depth) * w in the directional atlas

    // Point and spot lights: Scene_LocalCounts.x of them, six vec4s each.
    vec4 Scene_LocalCounts;      // x: how many
    vec4 Scene_Locals[96];       // [6i] xyz position, w range; [6i+1] rgb colour (display), a intensity;
                                 // [6i+2] xyz direction (spot), w type (0 point, 1 spot);
                                 // [6i+3] x cos outer, y cos inner, z first shadow matrix (-1 none), w technique;
                                 // [6i+4] rgb Modulated tint; [6i+5] x depth bias, y normal bias
    mat4 Scene_LocalShadowMatrices[24]; // world to (u, v, depth) * w in the local atlas; a point light's six faces +X -X +Y -Y +Z -Z
    vec4 Scene_CascadeTexels;    // each cascade's texel, world units: the normal offset is at least a texel and a half

    // HDR (ToneMapping.glsl): x 1 if the scene colour holds linear light (else display space, 8-bit);
    // y the exposure multiplier, z the tone map operator (0 None, 1 Reinhard, 2 ACES) -- for the
    // tone map pass and the background, which must come out of it as authored.
    vec4 Scene_Output;

    // Image-based lighting (IBLCommon.glsl): x 1 if the probe atlas lights this view -- the
    // Lighting stage's hemisphere ambient then gives way to it; y the probes; z the sky block's weight.
    vec4 Scene_IBL;
    // Forward Lit surfaces' screen-space reflections (ForwardReflections.glsl; LIT_TRANSLUCENCY.md,
    // Phase 4), set for the translucent stage: the last frame's view, unjittered, GL clip space;
    // [0] x intensity, y most roughness, z steps, w thickness; [1] x longest ray, y 1 if on, zw the
    // view's size. (Where the probes once were, before the light block held them.)
    mat4 Scene_PrevViewProjection;
    vec4 Scene_ForwardReflections[2];
    vec4 Scene_Spare[6];

    // Baked lighting (BakedLighting.glsl): x 1 if the view draws with a bake; y 1 if that bake
    // has the sky in it, so a baked surface takes no live sky ambient; z 1 if its lightmaps
    // are directional (the light's direction in each atlas's bottom half).
    vec4 Scene_Baked;

    // The view mode (RenderTypes.h's ERenderViewMode): x 1 for Unlit -- base colour and emissive, no light;
    // a material preview's channel (EMaterialPreviewChannel): 2 base colour alone, 3 emissive alone / 16.
    vec4 Scene_View;

    // Contact shadows (Lighting.glsl's ContactShadow; the Lighting stage only): world to clip,
    // GL's (Scene_InvViewProjection's inverse); x the march's length toward the sun, world units
    // (0: off), y a depth-buffer surface's assumed thickness, z its samples, w its strength.
    mat4 Scene_ViewProjection;
    vec4 Scene_ContactShadow;
    vec4 Scene_ContactLocal;     // x: 1 if shadowed point and spot lights march too

    // Occlusion reveal (MaterialTemplate_PS.glsl; RenderCore/RENDERPIPELINE.md, "Revealing
    // players"): x how many points, y the view's aspect (width / height), z the share of
    // pixels kept in a circle's middle, w how much nearer than a point (view depth) a
    // surface must be to be dithered. Each point: xy NDC (+Y up), z its circle's radius in
    // NDC y, w its view depth.
    vec4 Scene_RevealParams;
    vec4 Scene_Reveal[4];
UBO_BLOCK_END(Scene_Block)
