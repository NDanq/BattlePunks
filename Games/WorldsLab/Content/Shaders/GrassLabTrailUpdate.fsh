#version 450 core
// GrassLabTrailUpdate.fsh -- GrassLab's trail (WorldsLabGrassLab.cpp), a top-down
// map of the grass field: where the ball has pushed the grass, and which way.
// Each frame the last frame's trail eases back to rest -- the grass springs
// up -- and the ball, while it touches the ground, presses a soft disc into
// it: the blades under it pushed out from its centre and along its roll.
//
// A texel is (push.xy, flatten, 1): push the direction times how hard (world
// X, Z, -1..1), flatten 0..1. UV is the field: world (X, Z) = Field.xy + UV * Field.zw.

#include <PostProcessCommon.glsl>

MATERIAL_SAMPLER2D(4, Previous)

UBO_BLOCK_BEGIN(2, PP_Block)
    vec4 PP_Ball;       // xyz: the ball's centre, w: its radius (world units)
    vec4 PP_Motion;     // xy: its roll along the ground, 0..1 (X, Z); z: 1 touching the ground, 0 in the air
    vec4 PP_Settings;   // x: seconds since the last frame; y: recovery time constant (s); z: reach, in radii; w: 1 to clear
    vec4 PP_Unused;
    vec4 PP_Field;      // xy: the field's lowest world X, Z; zw: its size
    vec4 PP_Unused2;
UBO_BLOCK_END(PP_Block)

void main(void)
{
    vec2 UV = Interpolants.TexCoord;
    vec3 Trail = PP_Settings.w > 0.5 ? vec3(0.0) : texture(Previous, UV).xyz;

    // Back toward rest: what was pushed a time constant ago is a third of it now.
    Trail *= exp(-PP_Settings.x / max(PP_Settings.y, 0.01));

    vec2  World  = PP_Field.xy + UV * PP_Field.zw;
    vec2  Offset = World - PP_Ball.xz;
    float Dist   = length(Offset);
    float Reach  = PP_Ball.w * PP_Settings.z;
    float Touch  = PP_Motion.z * (1.0 - smoothstep(Reach * 0.5, Reach, Dist));
    if(Touch > 0.0)
    {
        // Out from under the ball, leaning the way it rolls.
        vec2  Out  = Dist > 0.001 ? Offset / Dist : vec2(0.0);
        vec2  Push = Out + PP_Motion.xy * 0.75;
        float Size = length(Push);
        Push = Size > 0.001 ? Push / Size : vec2(0.0);
        Trail.xy = mix(Trail.xy, Push, Touch);
        Trail.z  = max(Trail.z, Touch);
    }
    Output = vec4(Trail, 1.0);
}
