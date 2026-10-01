#if WITH_OPENGL
#include <OpenGLCommon.glsl>
#endif
#if WITH_VULKAN
#include <VulkanCommon.glsl>
#endif

struct GDIViewParameters
{
    mat4 Projection;
    mat4 View;
    vec3 MouseCoords;       // vec3(MouseX, MouseY, MouseDown)
    vec4 Resolution;        // vec4(ViewportWidth, ViewportHeight, 1.0/ViewportWidth, 1.0/ViewportHeight)
};

struct GDITransform
{
    vec3 PositionWorldSpace;
    mat4 Model;
};

// GDIVSToPSInterpolants is kept as a struct so GetInterpolants() can return it
// by value.  The actual VS->PS interface uses a GLSL interface block (below),
// which is valid on both OpenGL 4.x core and Vulkan without any #if guards.
//
// Normal is world space and LAST, so older pixel shaders read a valid prefix.
// Shaders without one (quads, the blit) write -Z.
struct GDIVSToPSInterpolants
{
    vec3 Position;
    vec2 TexCoord;
    vec3 Normal;
};

#if VERTEX_SHADER
layout(location=0) out Interp_Block
{
    vec3 Position;
    vec2 TexCoord;
    vec3 Normal;
} Interpolants;

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord, vec3 InNormal )
{
    Interpolants.Position = InPosition;
    Interpolants.TexCoord = InTexCoord;
    Interpolants.Normal   = InNormal;
}

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord )
{
    CalculateInterpolants(InPosition, InTexCoord, vec3(0.0, 0.0, -1.0));
}
#endif

#if PIXEL_SHADER
layout(location=0) in Interp_Block
{
    vec3 Position;
    vec2 TexCoord;
    vec3 Normal;
} Interpolants;

GDIVSToPSInterpolants GetInterpolants()
{
    GDIVSToPSInterpolants Out;
    Out.Position = Interpolants.Position;
    Out.TexCoord = Interpolants.TexCoord;
    Out.Normal   = Interpolants.Normal;
    return Out;
}
#endif

// Core uniform blocks — binding 0 and 1 are reserved on all platforms.
//
// OpenGL keeps the simple one-liner: DECLARE_UBO there expands to a plain
// `uniform GDIViewParameters ViewParameters;` (OpenGLCommon.glsl), which is
// legal GLSL and never passed through SPIR-V-Cross at all.
//
// Vulkan/D3D12 declare the block FLAT (one field per line, no nested
// struct), then a local variable of the original struct type copied from
// those fields, so every call site below this file (Basic.vsh's
// `ViewParameters.Projection`, etc.) keeps working unchanged. See
// VulkanCommon.glsl's own comment on why this could not stay a generic
// DECLARE_UBO macro under cook_shaders.py's preprocessor.
#if WITH_OPENGL
DECLARE_UBO(0, VP_Block, GDIViewParameters, ViewParameters)
DECLARE_UBO(1, TR_Block, GDITransform,      Transform)
#endif

#if WITH_VULKAN
layout(std140, set=0, binding=0) uniform VP_Block { mat4 Projection; mat4 View; vec3 MouseCoords; vec4 Resolution; };
GDIViewParameters ViewParameters = GDIViewParameters(Projection, View, MouseCoords, Resolution);

layout(std140, set=0, binding=1) uniform TR_Block { vec3 PositionWorldSpace; mat4 Model; };
GDITransform Transform = GDITransform(PositionWorldSpace, Model);
#endif
