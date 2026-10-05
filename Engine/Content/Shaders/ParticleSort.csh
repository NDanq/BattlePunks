#version 450
// ParticleSort.csh -- one workgroup: an emitter's particles sorted back to
// front along the view (a bitonic sort in shared memory, two a thread), then
// written as instances (GDIBuffer.h: a mat4 transform and a colour each), so
// translucent particles blend in order in one instanced draw
// (RenderCore/RENDERPIPELINE.md, "Particles").

#include <ParticleCommon.glsl>

#define SORT_MAX 2048
layout(local_size_x = 1024) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };

struct Instance
{
    mat4 Transform;
    vec4 Color;
};
STORAGE_BUFFER(2, InstanceData) { Instance Instances[]; };

shared float Keys[SORT_MAX];
shared uint  Ids[SORT_MAX];

void main()
{
    uint Thread = gl_LocalInvocationIndex;
    uint Max    = Pa_Spawn.z;
    uint Count  = Pa_Sort.x;   // a power of two >= Max, <= SORT_MAX

    // Farthest first: the key is minus the depth; the dead and the padding last.
    for(uint Slot = Thread; Slot < Count; Slot += 1024u)
    {
        float Key = 3.0e38;
        if(Slot < Max)
        {
            Particle P = Particles[Slot];
            if(IsAlive(P)) { Key = -dot(P.PositionAge.xyz - Pa_Camera.xyz, Pa_CameraForward.xyz); }
        }
        Keys[Slot] = Key;
        Ids[Slot]  = Slot;
    }
    barrier();

    for(uint Size = 2u; Size <= Count; Size <<= 1u)
    {
        for(uint Stride = Size >> 1u; Stride > 0u; Stride >>= 1u)
        {
            for(uint Slot = Thread; Slot < Count; Slot += 1024u)
            {
                uint Other = Slot ^ Stride;
                if(Other > Slot)
                {
                    bool bAscending = (Slot & Size) == 0u;
                    if((Keys[Slot] > Keys[Other]) == bAscending)
                    {
                        float Key = Keys[Slot]; Keys[Slot] = Keys[Other]; Keys[Other] = Key;
                        uint  Id  = Ids[Slot];  Ids[Slot]  = Ids[Other];  Ids[Other]  = Id;
                    }
                }
            }
            barrier();
        }
    }

    for(uint Slot = Thread; Slot < Max; Slot += 1024u)
    {
        Particle P = Particles[Ids[Slot]];
        Instance Out;
        if(!IsAlive(P))
        {
            Out.Transform = mat4(0.0);   // collapses to a point: draws nothing
            Out.Color     = vec4(0.0);
        }
        else
        {
            float Age   = clamp(P.PositionAge.w / P.VelocityLife.w, 0.0, 1.0);
            float Size  = mix(Pa_Size.x, Pa_Size.y, Age);
            float Alpha = smoothstep(0.0, max(Pa_Size.z, 1e-4), Age) * (1.0 - smoothstep(1.0 - max(Pa_Size.w, 1e-4), 1.0, Age));
            vec4  Color = mix(Pa_StartColor, Pa_EndColor, pow(Age, Pa_Noise.z));
            Color.a    *= Alpha;

            // Facing the camera, its -Z toward the eye (a quad faces -Z); else upright.
            vec3 Right   = Pa_Camera.w > 0.5 ? Pa_CameraRight.xyz   : vec3(1.0, 0.0, 0.0);
            vec3 Up      = Pa_Camera.w > 0.5 ? Pa_CameraUp.xyz      : vec3(0.0, 1.0, 0.0);
            vec3 Forward = Pa_Camera.w > 0.5 ? Pa_CameraForward.xyz : vec3(0.0, 0.0, 1.0);
            Out.Transform = mat4(vec4(Right * Size, 0.0), vec4(Up * Size, 0.0), vec4(Forward * Size, 0.0),
                                 vec4(P.PositionAge.xyz, 1.0));
            Out.Color = Color;
        }
        Instances[Slot] = Out;
    }
}
