// Lighting.glsl -- a Lit material's shading: one directional light and a
// sky/ground ambient (SceneUniforms.glsl), GGX distribution, Smith-Schlick
// geometry and Schlick Fresnel. Colours arrive display space and are lit
// linear; the template encodes the result back.

const float LIGHTING_PI = 3.14159265;

vec3 ToLinear(vec3 InColor)  { return pow(max(InColor, vec3(0.0)), vec3(2.2)); }
vec3 ToDisplay(vec3 InColor) { return pow(max(InColor, vec3(0.0)), vec3(1.0 / 2.2)); }

// Linear radiance leaving the surface toward the eye, from the light and the ambient.
vec3 ShadeSurface(in MaterialSurface Surface, vec3 InWorldPosition, vec3 InNormal)
{
    vec3  Albedo    = ToLinear(Surface.BaseColor);
    float Metallic  = clamp(Surface.Metallic, 0.0, 1.0);
    float Roughness = clamp(Surface.Roughness, 0.04, 1.0);

    vec3 N = normalize(InNormal);
    vec3 V = normalize(Scene_CameraPosition.xyz - InWorldPosition);
    vec3 L = normalize(Scene_LightDirection.xyz);
    vec3 H = normalize(V + L);

    float NdotL = max(dot(N, L), 0.0);
    float NdotV = max(dot(N, V), 1e-4);
    float NdotH = max(dot(N, H), 0.0);
    float VdotH = max(dot(V, H), 0.0);

    // GGX / Trowbridge-Reitz.
    float Alpha  = Roughness * Roughness;
    float Alpha2 = Alpha * Alpha;
    float Denom  = NdotH * NdotH * (Alpha2 - 1.0) + 1.0;
    float D      = Alpha2 / (LIGHTING_PI * Denom * Denom);

    // Smith with Schlick-GGX, k for direct light.
    float K = (Roughness + 1.0) * (Roughness + 1.0) / 8.0;
    float G = (NdotV / (NdotV * (1.0 - K) + K)) * (NdotL / (NdotL * (1.0 - K) + K));

    // Schlick Fresnel; Specular 0.5 is a 4% dielectric.
    vec3 F0 = mix(vec3(0.08 * clamp(Surface.Specular, 0.0, 1.0)), Albedo, Metallic);
    vec3 F  = F0 + (vec3(1.0) - F0) * pow(1.0 - VdotH, 5.0);

    vec3 Specular = D * G * F / max(4.0 * NdotV * NdotL, 1e-4);
    vec3 Diffuse  = (vec3(1.0) - F) * (1.0 - Metallic) * Albedo / LIGHTING_PI;
    vec3 Direct   = (Diffuse + Specular) * ToLinear(Scene_LightColor.rgb) * Scene_LightDirection.w * NdotL;

    // +Y is down, so the sky is toward -Y.
    float Up      = clamp(-N.y * 0.5 + 0.5, 0.0, 1.0);
    vec3  Ambient = mix(ToLinear(Scene_GroundColor.rgb), ToLinear(Scene_SkyColor.rgb), Up) * Scene_SkyColor.a;
    vec3  Indirect = Ambient * (Albedo * (1.0 - Metallic) + F0 * (1.0 - Roughness) * 0.5) * Surface.AmbientOcclusion;

    return Direct + Indirect;
}
