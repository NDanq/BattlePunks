// MaterialCommon.glsl -- what a material function (<Name>.mtl.glsl, generated
// from a material graph) and the templates that include it share.
// See RenderCore/MATERIALS.md.

// Bumped when a change here or in the templates breaks shader caches cooked
// before it; RenderCore refuses a cache that disagrees (MaterialPermutation.h).
#define MATERIAL_TEMPLATE_VERSION 6

// What input nodes read. Each stage's template fills it; the pixel-only
// fields exist only in the pixel stage, so a pixel-only node in the vertex
// stage fails to compile even if the graph's own check missed it.
struct MaterialInputs
{
    vec2  TexCoord;
    vec4  Color;            // vertex colour; white without one
    vec3  WorldPosition;    // vertex stage: before World Position Offset
    vec3  WorldNormal;
#if PIXEL_SHADER
    vec3  ClipPosition;
    vec4  FragCoord;
    float ViewDepth;        // linear, world units in front of the camera
    float DeviceDepth;      // gl_FragCoord.z
#endif
};

// What the pixel function writes. Colours are display space (as authored);
// Lighting.glsl linearises what it lights.
struct MaterialSurface
{
    vec3  BaseColor;
    float Metallic;
    float Roughness;
    float Specular;
    vec3  Emissive;
    float Opacity;
    float OpacityMask;
    vec3  Normal;           // world space
    float AmbientOcclusion;
};

MaterialSurface DefaultMaterialSurface(in MaterialInputs Inputs)
{
    MaterialSurface Surface;
    Surface.BaseColor        = vec3(1.0);
    Surface.Metallic         = 0.0;
    Surface.Roughness        = 0.5;
    Surface.Specular         = 0.5;
    Surface.Emissive         = vec3(0.0);
    Surface.Opacity          = 1.0;
    Surface.OpacityMask      = 1.0;
    Surface.Normal           = Inputs.WorldNormal;
    Surface.AmbientOcclusion = 1.0;
    return Surface;
}

// The scene drawn so far, for a PostProcess material's Scene Texture node:
// material texture slot 0; and its depth, for Scene Depth: slot 7.
#if PIXEL_SHADER && MATERIAL_DOMAIN_POSTPROCESS
MATERIAL_SAMPLER2D(4, Material_SceneColor)
MATERIAL_SAMPLER2D(11, Material_SceneDepth)

// Window depth, 0 near to 1 far.
float Material_SceneDepthAt(vec2 InFragCoord)
{
    return texelFetch(Material_SceneDepth, ivec2(InFragCoord), 0).r;
}

// World units from the eye to the scene at this pixel. InClip is the
// full-screen quad's position: this pixel's NDC.
float Material_SceneDistanceAt(vec2 InClip, vec2 InFragCoord)
{
    vec4 World = Scene_InvViewProjection * vec4(InClip, Material_SceneDepthAt(InFragCoord) * 2.0 - 1.0, 1.0);
    return length(World.xyz / World.w - Scene_CameraPosition.xyz);
}
#endif
