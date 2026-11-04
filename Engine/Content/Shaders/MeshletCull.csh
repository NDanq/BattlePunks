#version 450
// MeshletCull.csh -- GPU meshlet culling (RenderCore/RENDERPIPELINE.md,
// "Meshlets"): one thread a meshlet of one section (a record), a workgroup's
// row of them along x, the records along y. A meshlet whose sphere is outside
// one of the view's planes (or, with cones on, all of whose triangles face
// away; or, with a depth pyramid, that was behind the last frame's depth)
// writes nothing; a survivor takes room for its triangles in its
// section's region of the index buffer (an atomic add on its draw's index
// count) and writes their corners there as the mesh's own vertex indices.
// The section's Base Pass draw reads that count and those indices.

#include <ComputeCommon.glsl>

layout(local_size_x = 64) in;

COMPUTE_PARAMS_BEGIN(MeshletCullParams)
    vec4  Mc_Planes[6];      // the view's, world space, inward: dot(N, P) + D >= 0 inside
    vec4  Mc_Eye;            // xyz: the view's position; w: 1 to cull by the normal cones
    uvec4 Mc_Layout;         // first record, records, vertex base (words), triangle base (words)
    vec4  Mc_HzbRows[4];     // the depth pyramid's view projection, by row
    uvec4 Mc_Hzb;            // levels (0: no pyramid), the first level's width and height
    uvec4 Mc_HzbLevels[16];  // each level's offset (floats), width, height
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
// The last frame's depth pyramid (MeshletHzbFirst.csh, MeshletHzbDown.csh); anything when Mc_Hzb.x is 0.
STORAGE_BUFFER(5, MeshletPyramid) { float Pyramid[]; };

vec3 Place(Record InRecord, vec3 InPoint)
{
    vec4 Point = vec4(InPoint, 1.0);
    return vec3(dot(InRecord.Row0, Point), dot(InRecord.Row1, Point), dot(InRecord.Row2, Point));
}

float PyramidAt(uint InLevel, uvec2 InTexel)
{
    uvec4 Level = Mc_HzbLevels[InLevel];
    return Pyramid[Level.x + InTexel.y * Level.y + InTexel.x];
}

// Whether the box round a sphere was wholly behind the last frame's depth:
// its corners through the pyramid's view, the level where the box spans two
// texels at most, the farthest depth of those against the box's nearest.
// Not if any corner was behind the eye, or the box was off that view.
bool HiddenLastFrame(vec3 InCentre, float InRadius)
{
    vec2  UvMin   = vec2(1.0), UvMax = vec2(0.0);
    float Nearest = 1.0;
    for(int Corner = 0; Corner < 8; ++Corner)
    {
        vec3 Offset = vec3((Corner & 1) != 0 ? InRadius : -InRadius, (Corner & 2) != 0 ? InRadius : -InRadius,
                           (Corner & 4) != 0 ? InRadius : -InRadius);
        vec4 Point = vec4(InCentre + Offset, 1.0);
        vec4 Clip  = vec4(dot(Mc_HzbRows[0], Point), dot(Mc_HzbRows[1], Point), dot(Mc_HzbRows[2], Point), dot(Mc_HzbRows[3], Point));
        if(Clip.w <= 1e-4)
        {
            return false;
        }
        vec3 Ndc = Clip.xyz / Clip.w;
        vec2 Uv  = vec2(Ndc.x * 0.5 + 0.5, 0.5 - Ndc.y * 0.5);   // row 0 is the top
        UvMin   = min(UvMin, Uv);
        UvMax   = max(UvMax, Uv);
        // As the depth buffer holds it: clip z taken as -w to w, written 0 to 1.
        Nearest = min(Nearest, Ndc.z * 0.5 + 0.5);
    }
    if(UvMin.x < 0.0 || UvMin.y < 0.0 || UvMax.x > 1.0 || UvMax.y > 1.0 || Nearest <= 0.0)
    {
        return false;
    }

    vec2  Span  = (UvMax - UvMin) * vec2(Mc_Hzb.yz);
    uint  Level = min(uint(ceil(log2(max(max(Span.x, Span.y), 1.0)))), Mc_Hzb.x - 1u);
    uvec2 Size  = Mc_HzbLevels[Level].yz;
    uvec2 Lo    = min(uvec2(UvMin * vec2(Size)), Size - 1u);
    uvec2 Hi    = min(uvec2(UvMax * vec2(Size)), Size - 1u);
    float Far   = max(max(PyramidAt(Level, Lo), PyramidAt(Level, uvec2(Hi.x, Lo.y))),
                      max(PyramidAt(Level, uvec2(Lo.x, Hi.y)), PyramidAt(Level, Hi)));
    return Nearest > Far;
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

    // Behind what was drawn the last frame (the depth pyramid): a frame late
    // where it comes out from behind something, as the CPU's queries are.
    if(Mc_Hzb.x != 0u && HiddenLastFrame(Centre, Radius))
    {
        return;
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
