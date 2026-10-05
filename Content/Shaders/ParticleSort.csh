#version 450
// ParticleSort.csh -- one workgroup: a sprite or mesh emitter's particles
// sorted back to front along the view (a bitonic sort in shared memory, two a
// thread), then written as instances (GDIBuffer.h: a mat4 transform and a
// colour each), so translucent particles blend in order in one instanced draw
// (RenderCore/RENDERPIPELINE.md, "Particles"). The living sort first; their
// count goes into the draws' arguments, so only they are drawn. Each is turned as its type
// says: a sprite by its billboard, a mesh by its orientation.

#include <ParticleCommon.glsl>

#define SORT_MAX 2048
layout(local_size_x = 1024) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };

STORAGE_BUFFER(2, InstanceData) { Instance Instances[]; };
STORAGE_BUFFER(3, DrawArgs) { uint Args[]; };

// InVector turned InAngle radians about the unit InAxis (Rodrigues).
vec3 Rotate(vec3 InVector, vec3 InAxis, float InAngle)
{
    float C = cos(InAngle), S = sin(InAngle);
    return InVector * C + cross(InAxis, InVector) * S + InAxis * dot(InAxis, InVector) * (1.0 - C);
}

// A unit vector at right angles to InDirection.
vec3 AnyPerpendicular(vec3 InDirection)
{
    vec3 Other = abs(InDirection.y) < 0.95 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    return normalize(cross(Other, InDirection));
}

// A sprite's or mesh's transform: its axes, sized, at its position. Column
// X x Y is Z, as the camera's are, so a quad (facing -Z) faces the eye.
mat4 ParticleTransform(Particle InParticle, float InSize)
{
    vec3 Position = InParticle.PositionAge.xyz;
    vec3 Velocity = InParticle.VelocityLife.xyz;
    float Speed   = length(Velocity);
    float Age     = InParticle.PositionAge.w;

    // Its own random start angle and spin direction, fixed at birth.
    uint  Seed  = floatBitsToUint(InParticle.Extra.x);
    float Start = 0.0, Spin = 1.0;
    if(Pa_Render.w != 0u)
    {
        Start = HashFloat(Seed) * 6.2831853;
        Spin  = HashFloat(Seed) < 0.5 ? -1.0 : 1.0;
    }
    float Angle = Start + Spin * Pa_Motion.x * Age;

    vec3 Right = vec3(1.0, 0.0, 0.0), Up = vec3(0.0, 1.0, 0.0), Forward = vec3(0.0, 0.0, 1.0);
    float Length = InSize;
    if(Pa_Render.x == PARTICLE_MESH)
    {
        if(Pa_Render.z == MESH_ALIGN_TO_VELOCITY && Speed > 1e-4)
        {
            Forward = Velocity / Speed;
            Right   = AnyPerpendicular(Forward);
            Up      = cross(Forward, Right);
            Right   = Rotate(Right, Forward, Angle);
            Up      = Rotate(Up, Forward, Angle);
        }
        else
        {
            // Fixed spins about +Y; Random Spin about an axis of its own.
            vec3 Axis = vec3(0.0, 1.0, 0.0);
            if(Pa_Render.z == MESH_RANDOM_SPIN)
            {
                float Z   = HashFloat(Seed) * 2.0 - 1.0;
                float Phi = HashFloat(Seed) * 6.2831853;
                float R   = sqrt(max(1.0 - Z * Z, 0.0));
                Axis = vec3(R * cos(Phi), Z, R * sin(Phi));
            }
            Right   = Rotate(Right, Axis, Angle);
            Up      = Rotate(Up, Axis, Angle);
            Forward = Rotate(Forward, Axis, Angle);
        }
    }
    else
    {
        vec3 ToEye = Pa_Camera.xyz - Position;
        if(Pa_Render.y == BILLBOARD_FACE_CAMERA)
        {
            Right = Pa_CameraRight.xyz; Up = Pa_CameraUp.xyz; Forward = Pa_CameraForward.xyz;
        }
        else if(Pa_Render.y == BILLBOARD_VERTICAL)
        {
            vec3 Flat = vec3(-ToEye.x, 0.0, -ToEye.z);
            Forward = dot(Flat, Flat) > 1e-8 ? normalize(Flat) : vec3(Pa_CameraForward.x, 0.0, Pa_CameraForward.z + 1e-6);
            Forward = normalize(Forward);
            Up      = vec3(0.0, 1.0, 0.0);
            Right   = cross(Up, Forward);
        }
        else if(Pa_Render.y == BILLBOARD_VELOCITY_ALIGNED)
        {
            // Up along its motion, longer the faster it goes; turned about it to the eye. No spin.
            Up      = Speed > 1e-4 ? Velocity / Speed : Pa_CameraUp.xyz;
            vec3 Away = dot(ToEye, ToEye) > 1e-8 ? -normalize(ToEye) : Pa_CameraForward.xyz;
            vec3 Side = cross(Up, Away);
            Right   = dot(Side, Side) > 1e-8 ? normalize(Side) : Pa_CameraRight.xyz;
            Forward = cross(Right, Up);
            Length  = InSize + Speed * Pa_Motion.y;
            Angle   = 0.0;
        }
        // Spun in its own plane.
        vec3 Turned = Right * cos(Angle) + Up * sin(Angle);
        Up    = Up * cos(Angle) - Right * sin(Angle);
        Right = Turned;
    }
    return mat4(vec4(Right * InSize, 0.0), vec4(Up * Length, 0.0), vec4(Forward * InSize, 0.0), vec4(Position, 1.0));
}

shared uint  AliveCount;
shared float Keys[SORT_MAX];
shared uint  Ids[SORT_MAX];

void main()
{
    uint Thread = gl_LocalInvocationIndex;
    uint Max    = Pa_Spawn.z;
    uint Count  = Pa_Sort.x;   // a power of two >= Max, <= SORT_MAX

    if(Thread == 0u) { AliveCount = 0u; }
    barrier();

    // Farthest first: the key is minus the depth; the dead and the padding last.
    for(uint Slot = Thread; Slot < Count; Slot += 1024u)
    {
        float Key = 3.0e38;
        if(Slot < Max)
        {
            Particle P = Particles[Slot];
            if(IsAlive(P))
            {
                Key = -dot(P.PositionAge.xyz - Pa_Camera.xyz, Pa_CameraForward.xyz);
                atomicAdd(AliveCount, 1u);
            }
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
            float Age = NormalizedAge(P);
            Out.Transform = ParticleTransform(P, SizeAt(Age));
            Out.Color     = ColorAt(Age);

            // A flipbook's frame rides in the transform's spare bottom row (frame, columns,
            // rows), which the material's InstancedMesh vertex shader reads and clears.
            if(Pa_Flipbook.x > 0.5)
            {
                float Frames = Pa_Flipbook.z;
                uint  Seed   = floatBitsToUint(P.Extra.x) ^ 0x68E31DA4u;
                float Start  = Pa_Size.z > 0.5 ? floor(HashFloat(Seed) * Frames) : 0.0;
                float Played = Pa_Flipbook.w > 0.0 ? P.PositionAge.w * Pa_Flipbook.w   // frames a second
                                                   : Age * Frames * Pa_Size.w;         // cycles over its life
                float Frame  = mod(Start + floor(min(Played, 1.0e6)), Frames);
                Out.Transform[0].w = Frame;
                Out.Transform[1].w = Pa_Flipbook.x;
                Out.Transform[2].w = Pa_Flipbook.y;
            }
        }
        Instances[Slot] = Out;
    }

    // The living sort first: the draws draw that many (and the dead after them never).
    if(Thread == 0u)
    {
        for(uint Draw = 0u; Draw < Pa_Sort.w; ++Draw) { Args[Draw * ARGS_WORDS + 1u] = AliveCount; }
    }
}
