#version 450 core
// ShadowCacheCopy.fsh -- the shadow cache (RenderCore/RENDERPIPELINE.md, "Shadow cache"): a
// shadowed point or spot light's tile of static casters' depth, copied from the cache atlas
// into the local shadow atlas before its movable casters are drawn over it -- or the far plane,
// clearing a cache tile before its static casters are drawn into it. Drawn over the tile with
// the full-screen vertex shader, the depth test passing everywhere and writing; the colour
// target is masked off. Both atlases are stored top-down on every backend, so the cache texel
// is this one's window position plus a whole-texel offset.

#include <PostProcessCommon.glsl>

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Copy;   // xy: the cache texel less this one; z: 1 copies, 0 writes the far plane
UBO_BLOCK_END(PP_Block)

MATERIAL_SAMPLER2D(4, Source)   // the cache atlas's depth

void main(void)
{
    float Depth = 1.0;
    if(PP_Copy.z > 0.5)
    {
        Depth = texelFetch(Source, ivec2(gl_FragCoord.xy) + ivec2(PP_Copy.xy), 0).r;
    }
    gl_FragDepth = Depth;
    Output = vec4(0.0);
}
