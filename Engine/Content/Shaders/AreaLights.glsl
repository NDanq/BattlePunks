// AreaLights.glsl -- polygonal lights by linearly transformed cosines (Heitz,
// Dupuy, Hill, Neubelt 2016; RenderCore/GLOBAL_ILLUMINATION.md, Phase 3): a
// BRDF's lobe is a clamped cosine under a 3 x 3 transform, so its integral
// over a polygon is the cosine's over the polygon transformed back -- an edge
// sum after clipping to the horizon. The transforms are fitted to the
// renderer's GGX (fit_ltc.py) and read from LightList_LTC (LightList.glsl).
// Include after LightList.glsl.

#ifndef AREA_LIGHTS_GLSL
#define AREA_LIGHTS_GLSL

const float AREA_PI = 3.14159265;

// The clamped cosine's integral over one edge of a polygon on the unit sphere.
float Area_IntegrateEdge(vec3 InA, vec3 InB)
{
    float CosTheta = clamp(dot(InA, InB), -0.9999, 0.9999);
    float Theta    = acos(CosTheta);
    return cross(InA, InB).z * (Theta > 0.001 ? Theta / sin(Theta) : 1.0);
}

// A quad (L[0..3]) clipped to the upper hemisphere (z > 0), in place: OutCount
// corners, 0 or 3 to 5 (Heitz et al.'s case table); L[OutCount] closes it.
void Area_ClipQuadToHorizon(inout vec3 L[5], out int OutCount)
{
    int Config = (L[0].z > 0.0 ? 1 : 0) + (L[1].z > 0.0 ? 2 : 0) + (L[2].z > 0.0 ? 4 : 0) + (L[3].z > 0.0 ? 8 : 0);
    OutCount = 0;
    if(Config == 1)       { OutCount = 3; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if(Config == 2)  { OutCount = 3; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if(Config == 3)  { OutCount = 4; L[2] = -L[2].z * L[1] + L[1].z * L[2]; L[3] = -L[3].z * L[0] + L[0].z * L[3]; }
    else if(Config == 4)  { OutCount = 3; L[0] = -L[3].z * L[2] + L[2].z * L[3]; L[1] = -L[1].z * L[2] + L[2].z * L[1]; }
    else if(Config == 6)  { OutCount = 4; L[0] = -L[0].z * L[1] + L[1].z * L[0]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if(Config == 7)  { OutCount = 5; L[4] = -L[3].z * L[0] + L[0].z * L[3]; L[3] = -L[3].z * L[2] + L[2].z * L[3]; }
    else if(Config == 8)  { OutCount = 3; L[0] = -L[0].z * L[3] + L[3].z * L[0]; L[1] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = L[3]; }
    else if(Config == 9)  { OutCount = 4; L[1] = -L[1].z * L[0] + L[0].z * L[1]; L[2] = -L[2].z * L[3] + L[3].z * L[2]; }
    else if(Config == 11) { OutCount = 5; L[4] = L[3]; L[3] = -L[2].z * L[3] + L[3].z * L[2]; L[2] = -L[2].z * L[1] + L[1].z * L[2]; }
    else if(Config == 12) { OutCount = 4; L[1] = -L[1].z * L[2] + L[2].z * L[1]; L[0] = -L[0].z * L[3] + L[3].z * L[0]; }
    else if(Config == 13) { OutCount = 5; L[4] = L[3]; L[3] = L[2]; L[2] = -L[1].z * L[2] + L[2].z * L[1]; L[1] = -L[1].z * L[0] + L[0].z * L[1]; }
    else if(Config == 14) { OutCount = 5; L[4] = -L[0].z * L[3] + L[3].z * L[0]; L[0] = -L[0].z * L[1] + L[1].z * L[0]; }
    else if(Config == 15) { OutCount = 4; }
    if(OutCount == 3) { L[3] = L[0]; }
    if(OutCount == 4) { L[4] = L[0]; }
}

// The cosine lobe InMinv transforms, integrated over the quad InCorners (world
// space) seen from InPosition on a surface of normal InN viewed along InV: the
// share of the lobe the quad covers, 0 to 1. Either face lights.
float Area_LTCIntegrate(vec3 InN, vec3 InV, vec3 InPosition, mat3 InMinv, vec3 InCorners[4])
{
    // The surface's frame: x toward the eye's azimuth, z the normal.
    vec3 T1 = InV - InN * dot(InV, InN);
    float Length = length(T1);
    T1 = Length > 1e-4 ? T1 / Length : normalize(cross(InN, abs(InN.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
    vec3 T2 = cross(InN, T1);
    mat3 Minv = InMinv * transpose(mat3(T1, T2, InN));

    vec3 L[5];
    L[0] = Minv * (InCorners[0] - InPosition);
    L[1] = Minv * (InCorners[1] - InPosition);
    L[2] = Minv * (InCorners[2] - InPosition);
    L[3] = Minv * (InCorners[3] - InPosition);
    L[4] = L[3];
    int Count;
    Area_ClipQuadToHorizon(L, Count);
    if(Count == 0) { return 0.0; }
    L[0] = normalize(L[0]); L[1] = normalize(L[1]); L[2] = normalize(L[2]);
    L[3] = normalize(L[3]); L[4] = normalize(L[4]);
    float Sum = Area_IntegrateEdge(L[0], L[1]) + Area_IntegrateEdge(L[1], L[2]) + Area_IntegrateEdge(L[2], L[3]);
    if(Count >= 4) { Sum += Area_IntegrateEdge(L[3], L[4]); }
    if(Count == 5) { Sum += Area_IntegrateEdge(L[4], L[0]); }
    return abs(Sum) / (2.0 * AREA_PI);
}

// The fitted transform and amplitudes at a perceptual roughness and NdotV.
void Area_LTCLookup(float InRoughness, float InNdotV, out mat3 OutMinv, out vec2 OutAmplitude)
{
    vec2 Cell = vec2(clamp(InRoughness, 0.0, 1.0), sqrt(clamp(1.0 - InNdotV, 0.0, 1.0))) * (63.0 / 64.0) + 0.5 / 64.0;
    vec4 M = texture(LightList_LTC, vec2(Cell.x, Cell.y * 0.5));
    OutAmplitude = texture(LightList_LTC, vec2(Cell.x, 0.5 + Cell.y * 0.5)).xy;
    OutMinv = mat3(vec3(M.x, 0.0, M.y), vec3(0.0, 1.0, 0.0), vec3(M.z, 0.0, M.w));
}

#endif
