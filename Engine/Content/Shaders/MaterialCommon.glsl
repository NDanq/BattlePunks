// MaterialCommon.glsl -- what a material function (<Name>.mtl.glsl, generated
// from a material graph) and the templates that include it share.
// See RenderCore/MATERIALS.md.

// Bumped when a change here or in the templates breaks shader caches cooked
// before it; RenderCore refuses a cache that disagrees (MaterialPermutation.h).
#define MATERIAL_TEMPLATE_VERSION 8

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

// The Normal Map node: a tangent-space normal map's colour (0-1 per channel)
// as a world-space normal. No mesh carries tangents, so the frame comes from
// how the world position and InUV change across the pixel (Schueler's
// cotangent frame): T along +U, B along +V, N the interpolated normal.
// Verified on Vulkan by the RenderCore GPU smoke test (NormalProbe). Row 0
// of an image is V = 0, so the image's "up" is -V: an OpenGL map's green (Y+,
// up the image) takes InGreen -1, a DirectX map's (Y-, down it) +1.
#if PIXEL_SHADER
vec3 Material_NormalMap(in MaterialInputs Inputs, vec3 InColor, float InStrength, vec2 InUV, float InGreen)
{
    vec3 N = normalize(Inputs.WorldNormal);
    vec3 Tangent = InColor * 2.0 - 1.0;
    Tangent.y *= InGreen;
    Tangent.xy *= InStrength;

    vec3 Dp1  = dFdx(Inputs.WorldPosition);
    vec3 Dp2  = dFdy(Inputs.WorldPosition);
    vec2 Duv1 = dFdx(InUV);
    vec2 Duv2 = dFdy(InUV);
    vec3 Dp2Perp = cross(Dp2, N);
    vec3 Dp1Perp = cross(N, Dp1);
    vec3 T = Dp2Perp * Duv1.x + Dp1Perp * Duv2.x;
    vec3 B = Dp2Perp * Duv1.y + Dp1Perp * Duv2.y;
    float Scale = max(dot(T, T), dot(B, B));
    if(Scale < 1e-20)
    {
        return N;   // no UV change across the pixel: nothing to orient the map by
    }
    /* The solve is scaled by det(dPdx, dPdy, N), whose sign is the screen's
     * handedness against the normal: it differs between a left-handed world,
     * the backends' dFdy direction and a back face. Its sign puts T along +U. */
    Scale = inversesqrt(Scale) * (dot(cross(Dp1, Dp2), N) < 0.0 ? -1.0 : 1.0);
    return normalize(mat3(T * Scale, B * Scale, N) * Tangent);
}
#endif

// The Reflect node (RenderCore/RENDERPIPELINE.md, "Scene capture"): sampling
// a scene capture probe's render target.
#if PIXEL_SHADER
// A planar mirror's capture (RenderTargetTex2D) at this pixel: the probe
// renders the camera reflected in its plane, so a point on the mirror reads
// the capture at the horizontally mirrored screen position. Perspective
// views only (ViewDepth is clip w there).
vec2 Material_MirrorUV(in MaterialInputs Inputs)
{
    vec2 Ndc = Inputs.ClipPosition.xy / max(Inputs.ViewDepth, 1e-4);
    vec2 Uv  = Ndc * 0.5 + 0.5;
    return vec2(1.0 - Uv.x, Uv.y);
}

// The view direction reflected about InNormal: where an omnidirectional
// capture is sampled.
vec3 Material_ReflectionVector(in MaterialInputs Inputs, vec3 InNormal)
{
    vec3 ToEye = normalize(Scene_CameraPosition.xyz - Inputs.WorldPosition);
    return reflect(-ToEye, normalize(InNormal));
}

// An omnidirectional capture (RenderTargetTex3D) by direction: its six faces
// side by side, +X -X +Y -Y +Z -Z, each a 90-degree view looking along its
// axis with up +Y (+Z for the Y faces) -- SceneCapture.cpp's CubeFaceBasis.
// Each face's lookup stays half a texel inside it, so filtering never reads
// the next face.
vec4 Material_SampleCubeStrip(sampler2D InStrip, vec3 InDirection)
{
    vec3 A = abs(InDirection);
    int  Face;
    if(A.x >= A.y && A.x >= A.z) { Face = InDirection.x > 0.0 ? 0 : 1; }
    else if(A.y >= A.z)          { Face = InDirection.y > 0.0 ? 2 : 3; }
    else                         { Face = InDirection.z > 0.0 ? 4 : 5; }

    vec3 Forward = Face == 0 ? vec3(1.0, 0.0, 0.0) : Face == 1 ? vec3(-1.0, 0.0, 0.0)
                 : Face == 2 ? vec3(0.0, 1.0, 0.0) : Face == 3 ? vec3(0.0, -1.0, 0.0)
                 : Face == 4 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 0.0, -1.0);
    vec3 Up      = (Face == 2 || Face == 3) ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
    vec3 Right   = normalize(cross(Up, Forward));   // the engine's LookAt: R = Up x D, U = D x R
    Up           = cross(Forward, Right);

    float Depth = max(dot(InDirection, Forward), 1e-6);
    vec2  Ndc   = vec2(dot(InDirection, Right), dot(InDirection, Up)) / Depth;
    float Size  = float(textureSize(InStrip, 0).y);
    float Inset = 0.5 / Size;
    vec2  Uv    = clamp(Ndc * 0.5 + 0.5, vec2(Inset), vec2(1.0 - Inset));
    return texture(InStrip, vec2((float(Face) + Uv.x) / 6.0, Uv.y));
}
#endif
