// Skinning.glsl -- a skinned mesh's bones (RenderCore/ANIMATION.md).
//
// The palette is GDI's skin block (binding 12): per bone, the three rows of
// the 3 x 4 affine matrix that takes a bind-space vertex to its posed place
// in model space -- the bone's pose times its inverse bind (AnimMath.h's
// AnimMatrix, row-major). RenderCore sets it before every skinned draw; the
// vertex brings four bone indices (location 11, as floats) and weights (12).

#define SKIN_MAX_BONES 256

UBO_BLOCK_BEGIN(12, Skin_Block)
    vec4 Skin_Bones[SKIN_MAX_BONES * 3];
UBO_BLOCK_END(Skin_Block)

// The palette the frame before (binding 13): where the vertex was, for its
// velocity. RenderCore sets it with the palette; the same one if it is new.
UBO_BLOCK_BEGIN(13, PrevSkin_Block)
    vec4 PrevSkin_Bones[SKIN_MAX_BONES * 3];
UBO_BLOCK_END(PrevSkin_Block)

// The four bones' rows, blended by weight: one matrix to move the vertex by.
void SkinRows(vec4 InJoints, vec4 InWeights, out vec4 OutRow0, out vec4 OutRow1, out vec4 OutRow2)
{
    OutRow0 = vec4(0.0);
    OutRow1 = vec4(0.0);
    OutRow2 = vec4(0.0);
    for(int Each = 0; Each < 4; ++Each)
    {
        int Bone = clamp(int(InJoints[Each] + 0.5), 0, SKIN_MAX_BONES - 1) * 3;
        OutRow0 += Skin_Bones[Bone + 0] * InWeights[Each];
        OutRow1 += Skin_Bones[Bone + 1] * InWeights[Each];
        OutRow2 += Skin_Bones[Bone + 2] * InWeights[Each];
    }
}

// As SkinRows, from the frame before's palette.
void PrevSkinRows(vec4 InJoints, vec4 InWeights, out vec4 OutRow0, out vec4 OutRow1, out vec4 OutRow2)
{
    OutRow0 = vec4(0.0);
    OutRow1 = vec4(0.0);
    OutRow2 = vec4(0.0);
    for(int Each = 0; Each < 4; ++Each)
    {
        int Bone = clamp(int(InJoints[Each] + 0.5), 0, SKIN_MAX_BONES - 1) * 3;
        OutRow0 += PrevSkin_Bones[Bone + 0] * InWeights[Each];
        OutRow1 += PrevSkin_Bones[Bone + 1] * InWeights[Each];
        OutRow2 += PrevSkin_Bones[Bone + 2] * InWeights[Each];
    }
}

vec3 SkinPoint(vec4 InRow0, vec4 InRow1, vec4 InRow2, vec3 InPoint)
{
    vec4 P = vec4(InPoint, 1.0);
    return vec3(dot(InRow0, P), dot(InRow1, P), dot(InRow2, P));
}

// Exact for rotation and uniform scale, the poses skeletons have.
vec3 SkinDirection(vec4 InRow0, vec4 InRow1, vec4 InRow2, vec3 InDirection)
{
    return vec3(dot(InRow0.xyz, InDirection), dot(InRow1.xyz, InDirection), dot(InRow2.xyz, InDirection));
}
