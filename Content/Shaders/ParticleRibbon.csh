#version 450
// ParticleRibbon.csh -- a ribbon or beam emitter's strip, one thread a
// segment (RenderCore/RENDERPIPELINE.md, "Particles"). Its points are the
// particles from slot Pa_Sort.y round the ring, Pa_Sort.z of them, in the
// order they were born (a beam's: from the emitter to its end). Each point
// is widened across the view to its size at its age; each segment between
// two living points is two triangles, written as two instances: one of
// RibbonSegmentA.nmmesh into the first buffer, one of RibbonSegmentB.nmmesh
// into the second. A triangle's three corners map exactly through an affine
// transform, so neighbouring segments share their edges with no gap.

#include <ParticleCommon.glsl>

layout(local_size_x = 64) in;

STORAGE_BUFFER(1, ParticleState) { Particle Particles[]; };
STORAGE_BUFFER(2, InstanceDataA) { Instance InstancesA[]; };
STORAGE_BUFFER(3, InstanceDataB) { Instance InstancesB[]; };

Particle PointAt(uint InIndex) { return Particles[(Pa_Sort.y + InIndex) % Pa_Spawn.z]; }

// Point InIndex's two edges, left and right of its path as the eye sees it.
void Edges(uint InIndex, out vec3 OutLeft, out vec3 OutRight)
{
    uint     Count = Pa_Sort.z;
    Particle Here  = PointAt(InIndex);
    vec3     At    = Here.PositionAge.xyz;
    vec3     Back  = InIndex > 0u && IsAlive(PointAt(InIndex - 1u)) ? PointAt(InIndex - 1u).PositionAge.xyz : At;
    vec3     Ahead = InIndex + 1u < Count && IsAlive(PointAt(InIndex + 1u)) ? PointAt(InIndex + 1u).PositionAge.xyz : At;
    vec3     Along = Ahead - Back;
    if(dot(Along, Along) < 1e-10) { Along = vec3(0.0, 1.0, 0.0); }

    // Across the path, square to the eye: cross(edge, path) then points away from it, as a quad's does.
    vec3 Across = cross(Pa_Camera.xyz - At, Along);
    Across = dot(Across, Across) > 1e-10 ? normalize(Across) : Pa_CameraRight.xyz;
    vec3 Half = Across * (0.5 * SizeAt(NormalizedAge(Here)));
    OutLeft  = At - Half;
    OutRight = At + Half;
}

void main()
{
    uint Index = gl_GlobalInvocationID.x;
    if(Index >= Pa_Spawn.z) { return; }

    Instance A, B;
    A.Transform = mat4(0.0); A.Color = vec4(0.0);   // a dead segment collapses to a point
    B = A;
    if(Index + 1u < Pa_Sort.z && IsAlive(PointAt(Index)) && IsAlive(PointAt(Index + 1u)))
    {
        vec3 L0, R0, L1, R1;
        Edges(Index, L0, R0);
        Edges(Index + 1u, L1, R1);
        vec3 Away  = normalize(PointAt(Index).PositionAge.xyz - Pa_Camera.xyz + vec3(0.0, 0.0, 1e-6));
        vec4 Color = 0.5 * (ColorAt(NormalizedAge(PointAt(Index))) + ColorAt(NormalizedAge(PointAt(Index + 1u))));

        // A: (L0, R0, L1), UVs (0,0) (1,0) (0,1). B: (R1, L1, R0), UVs (1,1) (0,1) (1,0).
        A.Transform = mat4(vec4(R0 - L0, 0.0), vec4(L1 - L0, 0.0), vec4(Away, 0.0), vec4(L0, 1.0));
        B.Transform = mat4(vec4(L1 - R1, 0.0), vec4(R0 - R1, 0.0), vec4(Away, 0.0), vec4(R1, 1.0));
        A.Color = Color;
        B.Color = Color;
    }
    InstancesA[Index] = A;
    InstancesB[Index] = B;
}
