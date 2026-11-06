// ToneMapping.glsl -- HDR scene colour to display (RenderCore/RENDERPIPELINE.md,
// "HDR and tone mapping"). The scene colour is linear light when
// Scene_Output.x is 1; Tonemap.fsh turns it into display space by
// Scene_Output.y (exposure, a multiplier) and Scene_Output.z (the operator).
// The inverses let a background authored in display space (a clear colour,
// the editor's gradient) come out of the tone map as authored.

#define TONEMAP_NONE     0
#define TONEMAP_REINHARD 1
#define TONEMAP_ACES     2

// Narkowicz's fit of the ACES filmic curve, input scaled by 0.6 so 1.0 maps
// near 0.8 (the reference's mid-grey response).
vec3 ToneMapAces(vec3 X)
{
    X *= 0.6;
    return clamp((X * (2.51 * X + 0.03)) / (X * (2.43 * X + 0.59) + 0.14), 0.0, 1.0);
}

// Its inverse: Y = (2.51x^2 + 0.03x) / (2.43x^2 + 0.59x + 0.14), solved for x.
vec3 InverseToneMapAces(vec3 Y)
{
    Y = clamp(Y, 0.0, 0.999);
    vec3 A = 2.43 * Y - 2.51;
    vec3 B = 0.59 * Y - 0.03;
    vec3 C = 0.14 * Y;
    vec3 X = (-B - sqrt(max(B * B - 4.0 * A * C, 0.0))) / (2.0 * A);
    return max(X, 0.0) / 0.6;
}

vec3 ToneMap(vec3 InLinear, float InOperator)
{
    if(InOperator > 1.5) { return ToneMapAces(InLinear); }
    if(InOperator > 0.5) { return InLinear / (1.0 + InLinear); }
    return clamp(InLinear, 0.0, 1.0);
}

vec3 InverseToneMap(vec3 InMapped, float InOperator)
{
    if(InOperator > 1.5) { return InverseToneMapAces(InMapped); }
    if(InOperator > 0.5) { vec3 Y = clamp(InMapped, 0.0, 0.999); return Y / (1.0 - Y); }
    return InMapped;
}

// A display-space colour, as the scene colour must hold it to come out of the
// tone map unchanged: itself in an 8-bit scene, else linearised, un-tone-mapped
// and un-exposed.
vec3 SceneEncodeDisplay(vec3 InDisplay)
{
    if(Scene_Output.x < 0.5) { return InDisplay; }
    vec3 Linear = SrgbToLinear(InDisplay);
    return InverseToneMap(Linear, Scene_Output.z) / max(Scene_Output.y, 1e-6);
}
