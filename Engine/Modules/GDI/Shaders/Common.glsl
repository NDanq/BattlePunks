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
struct GDIVSToPSInterpolants
{
    vec3 Position;
    vec2 TexCoord;
};

#if VERTEX_SHADER
layout(location=0) out Interp_Block
{
    vec3 Position;
    vec2 TexCoord;
} Interpolants;

void CalculateInterpolants( vec3 InPosition, vec2 InTexCoord )
{
    Interpolants.Position = InPosition;
    Interpolants.TexCoord = InTexCoord;
}
#endif

#if PIXEL_SHADER
layout(location=0) in Interp_Block
{
    vec3 Position;
    vec2 TexCoord;
} Interpolants;

GDIVSToPSInterpolants GetInterpolants()
{
    GDIVSToPSInterpolants Out;
    Out.Position = Interpolants.Position;
    Out.TexCoord = Interpolants.TexCoord;
    return Out;
}
#endif

// Core uniform blocks — binding 0 and 1 are reserved on all platforms.
DECLARE_UBO(0, VP_Block, GDIViewParameters, ViewParameters)
DECLARE_UBO(1, TR_Block, GDITransform,      Transform)
