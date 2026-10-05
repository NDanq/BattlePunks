#version 450
// ParticleSimulate.csh -- one emitter's particles, one thread each: this
// frame's spawns born into their ring slots, the rest moved on by Delta
// (RenderCore/RENDERPIPELINE.md, "Particles"). Every type but a beam, whose
// points ParticleBeam.csh places instead.

#include <ParticleCommon.glsl>

layout(local_size_x = 64) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };

// A direction uniformly over the sphere, from two randoms.
vec3 RandomDirection(inout uint InOutSeed)
{
    float Z   = HashFloat(InOutSeed) * 2.0 - 1.0;
    float Phi = HashFloat(InOutSeed) * 6.2831853;
    float R   = sqrt(max(1.0 - Z * Z, 0.0));
    return vec3(R * cos(Phi), Z, R * sin(Phi));
}

// Where in the emitter's shape a particle is born, and the way out of the
// shape there, in the shape's own axes (+Y its up).
void ShapePoint(inout uint InOutSeed, out vec3 OutPoint, out vec3 OutNormal)
{
    uint  Shape   = uint(Pa_ShapeA.x);
    bool  Surface = Pa_ShapeA.y > 0.5;
    float Radius  = Pa_Emitter.w;
    if(Shape == SHAPE_BOX)
    {
        vec3 Half = Pa_ShapeB.xyz;
        vec3 P    = (vec3(HashFloat(InOutSeed), HashFloat(InOutSeed), HashFloat(InOutSeed)) * 2.0 - 1.0) * Half;
        // The nearest face (on the surface: a face chosen by its area, the point pushed onto it).
        vec3 Area  = vec3(Half.y * Half.z, Half.x * Half.z, Half.x * Half.y);
        float Pick = HashFloat(InOutSeed) * max(Area.x + Area.y + Area.z, 1e-6);
        int   Axis = Pick < Area.x ? 0 : (Pick < Area.x + Area.y ? 1 : 2);
        if(!Surface)
        {
            vec3 Depth = (Half - abs(P)) / max(Half, vec3(1e-6));
            Axis = Depth.x < Depth.y ? (Depth.x < Depth.z ? 0 : 2) : (Depth.y < Depth.z ? 1 : 2);
        }
        float Side = Surface ? (HashFloat(InOutSeed) < 0.5 ? -1.0 : 1.0) : (P[Axis] < 0.0 ? -1.0 : 1.0);
        if(Surface) { P[Axis] = Side * Half[Axis]; }
        OutPoint = P;
        OutNormal = vec3(0.0);
        OutNormal[Axis] = Side;
        return;
    }
    if(Shape == SHAPE_CONE)
    {
        // A disc facing +Y (its rim, on the surface); out within the cone's angle of +Y.
        float Phi = HashFloat(InOutSeed) * 6.2831853;
        float R   = Radius * (Surface ? 1.0 : sqrt(HashFloat(InOutSeed)));
        OutPoint  = vec3(cos(Phi) * R, 0.0, sin(Phi) * R);
        float CosT = mix(1.0, cos(Pa_ShapeA.w), HashFloat(InOutSeed));
        float SinT = sqrt(max(1.0 - CosT * CosT, 0.0));
        float Turn = HashFloat(InOutSeed) * 6.2831853;
        OutNormal  = vec3(cos(Turn) * SinT, CosT, sin(Turn) * SinT);
        return;
    }
    if(Shape == SHAPE_RING)
    {
        // A circle about +Y, Width across it (none, on the surface).
        float Phi = HashFloat(InOutSeed) * 6.2831853;
        vec3  Out = vec3(cos(Phi), 0.0, sin(Phi));
        float R   = Radius + (Surface ? 0.0 : (HashFloat(InOutSeed) - 0.5) * Pa_ShapeB.w);
        OutPoint  = Out * R;
        OutNormal = Out;
        return;
    }
    // A sphere or its upper half: anywhere within it, or on its shell.
    vec3 Dir = RandomDirection(InOutSeed);
    if(Shape == SHAPE_HEMISPHERE) { Dir.y = abs(Dir.y); }
    OutPoint  = Dir * (Radius * (Surface ? 1.0 : pow(HashFloat(InOutSeed), 1.0 / 3.0)));
    OutNormal = Dir;
}

// A smooth swirl in space and time: three phase-shifted sine waves, each
// axis driven by the other two -- cheap, divergence-ish, never still.
vec3 Swirl(vec3 InPosition, float InTime, float InFrequency)
{
    vec3 P = InPosition * InFrequency;
    return vec3(sin(P.y * 1.7 + InTime * 1.3) + sin(P.z * 1.1 - InTime * 0.7),
                sin(P.z * 1.3 + InTime * 0.9) + sin(P.x * 0.9 + InTime * 1.1) * 0.5,
                sin(P.x * 1.5 - InTime * 1.2) + sin(P.y * 1.2 + InTime * 0.8));
}

void main()
{
    uint Index = gl_GlobalInvocationID.x;
    uint Max   = Pa_Spawn.z;
    if(Index >= Max) { return; }

    float Delta = Pa_Timing.x;
    Particle P  = Particles[Index];

    // This frame's spawns are the Count slots from First round the ring, the
    // first born earliest: each at its own moment inside the frame (so a burst
    // is a stream, not a stack, and a ribbon's points come in the order they
    // join it), where the emitter was at that moment.
    uint Offset = (Index + Max - Pa_Spawn.x) % Max;
    if(Offset < Pa_Spawn.y)
    {
        uint  Seed     = HashUint(Index * 9781u + Pa_Spawn.w * 6271u);
        float Life     = Pa_Timing.y * (1.0 - Pa_Timing.z * HashFloat(Seed));
        float Age      = Delta * (float(Pa_Spawn.y - 1u - Offset) + HashFloat(Seed)) / float(Pa_Spawn.y);
        vec3  Emitter  = mix(Pa_Emitter.xyz, Pa_PrevEmitter.xyz, Delta > 0.0 ? Age / Delta : 0.0);
        vec3  Local, Outward;
        ShapePoint(Seed, Local, Outward);
        vec3  AxisX    = Pa_ShapeAxisX.xyz;
        vec3  AxisY    = Pa_ShapeAxisY.xyz;
        vec3  AxisZ    = cross(AxisX, AxisY);
        vec3  Position = Emitter + AxisX * Local.x + AxisY * Local.y + AxisZ * Local.z;
        vec3  Out      = AxisX * Outward.x + AxisY * Outward.y + AxisZ * Outward.z;
        vec3  Velocity = Pa_Velocity.xyz + Out * Pa_ShapeA.z + RandomDirection(Seed) * (Pa_Velocity.w * HashFloat(Seed));
        P.PositionAge  = vec4(Position + Velocity * Age, Age);
        P.VelocityLife = vec4(Velocity, max(Life, 1e-3));
        P.Extra        = vec4(uintBitsToFloat(HashUint(Seed ^ 0x9E3779B9u)), 0.0, 0.0, 0.0);
    }
    else if(IsAlive(P))
    {
        vec3 Velocity = P.VelocityLife.xyz;
        Velocity += (Pa_Acceleration.xyz + Swirl(P.PositionAge.xyz, Pa_Noise.x, Pa_Noise.y) * Pa_Timing.w) * Delta;
        Velocity *= exp(-Pa_Acceleration.w * Delta);
        P.PositionAge.xyz += Velocity * Delta;
        P.PositionAge.w   += Delta;
        P.VelocityLife.xyz = Velocity;
    }
    Particles[Index] = P;
}
