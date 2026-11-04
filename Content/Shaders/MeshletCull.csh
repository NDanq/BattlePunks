#version 450
// MeshletCull.csh -- GPU meshlet culling (RenderCore/RENDERPIPELINE.md,
// "Meshlets"): one thread a meshlet of one section (a record), a workgroup's
// row of them along x, the records along y. A meshlet whose sphere is outside
// one of the view's planes (or, with cones on, all of whose triangles face
// away) writes nothing; a survivor takes room for its triangles in its
// section's region of the index buffer (an atomic add on its draw's index
// count) and writes their corners there as the mesh's own vertex indices.
// The section's Base Pass draw reads that count and those indices.

#include <ComputeCommon.glsl>

layout(local_size_x = 64) in;

COMPUTE_PARAMS_BEGIN(MeshletCullParams)
    vec4  Mc_Planes[6];   // the view's, world space, inward: dot(N, P) + D >= 0 inside
    vec4  Mc_Eye;         // xyz: the view's position; w: 1 to cull by the normal cones
    uvec4 Mc_Layout;      // first record, records, vertex base (words), triangle base (words)
COMPUTE_PARAMS_END(MeshletCullParams)

// MeshletGpuMesh's: 12 words a meshlet, then its vertices, then its triangles' corners, four a word.
STORAGE_BUFFER(1, MeshletData) { uint Mesh[]; };

struct Record
{
    vec4  Row0;     // its placement's rows
    vec4  Row1;
    vec4  Row2;
    uvec4 Range;    // first meshlet, meshlets, first index, its arguments' slot
    vec4  Bounds;   // x: its largest axis scale, y: its World Position Offset's reach
};
STORAGE_BUFFER(2, MeshletRecords) { Record Records[]; };
STORAGE_BUFFER(3, MeshletIndices) { uint OutIndices[]; };
STORAGE_BUFFER(4, MeshletArgs) { uint Args[]; };

vec3 Place(Record InRecord, vec3 InPoint)
{
    vec4 Point = vec4(InPoint, 1.0);
    return vec3(dot(InRecord.Row0, Point), dot(InRecord.Row1, Point), dot(InRecord.Row2, Point));
}

void main()
{
    if(gl_WorkGroupID.y >= Mc_Layout.y)
    {
        return;
    }
    Record Section = Records[Mc_Layout.x + gl_WorkGroupID.y];
    uint Local = gl_GlobalInvocationID.x;
    if(Local >= Section.Range.y)
    {
        return;
    }

    uint  At     = (Section.Range.x + Local) * 12u;
    uvec4 Info   = uvec4(Mesh[At], Mesh[At + 1u], Mesh[At + 2u], Mesh[At + 3u]);   // vertex offset, vertices, triangle byte, triangles
    vec4  Sphere = uintBitsToFloat(uvec4(Mesh[At + 4u], Mesh[At + 5u], Mesh[At + 6u], Mesh[At + 7u]));
    vec4  Cone   = uintBitsToFloat(uvec4(Mesh[At + 8u], Mesh[At + 9u], Mesh[At + 10u], Mesh[At + 11u]));

    vec3  Centre = Place(Section, Sphere.xyz);
    float Radius = Sphere.w * Section.Bounds.x + Section.Bounds.y;
    for(int Plane = 0; Plane < 6; ++Plane)
    {
        if(dot(Mc_Planes[Plane].xyz, Centre) + Mc_Planes[Plane].w < -Radius)
        {
            return;
        }
    }

    // Every triangle faces away [Kapoulkine, meshoptimizer]: not with World
    // Position Offset (it moves them), nor a cone too wide to say.
    if(Mc_Eye.w != 0.0 && Cone.w < 1.0 && Section.Bounds.y == 0.0)
    {
        vec3 Axis = normalize(vec3(dot(Section.Row0.xyz, Cone.xyz), dot(Section.Row1.xyz, Cone.xyz), dot(Section.Row2.xyz, Cone.xyz)));
        vec3 Ray  = Centre - Mc_Eye.xyz;
        if(dot(Ray, Axis) >= Cone.w * length(Ray) + Radius)
        {
            return;
        }
    }

    uint Corners = Info.w * 3u;
    uint Out     = Section.Range.z + atomicAdd(Args[Section.Range.w * 5u], Corners);
    for(uint Corner = 0u; Corner < Corners; ++Corner)
    {
        uint Byte  = Info.z + Corner;
        uint Word  = Mesh[Mc_Layout.w + (Byte >> 2u)];
        uint Own   = (Word >> ((Byte & 3u) * 8u)) & 0xFFu;   // its meshlet's vertex
        OutIndices[Out + Corner] = Mesh[Mc_Layout.z + Info.x + Own];
    }
}
