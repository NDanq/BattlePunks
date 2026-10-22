# Shading models

Status: **Phases 0-5 landed; Phase 6 partly.** Clear Coat, Anisotropic,
Cloth, Subsurface and Skin draw in the Base Pass and forward, on Vulkan and
D3D12 (OpenGL compiles; Metal keeps an RGBA8 normal target). Still open:
a mesh tangent stream, the white furnace check, and a measured cost. See
[Open](#open).

A Lit material shaded with one BRDF before this: GGX, Smith-Schlick and
Schlick Fresnel over a Lambert diffuse. That covers plastics, metals, stone
and wood. It cannot draw car paint (a clear lacquer over a coloured base),
brushed metal (highlights stretched along the grain), cloth (a soft sheen at
grazing angles) or anything light passes into (skin, wax, leaves). A **shading
model** is a choice on the Material Output, beside Domain and Shading.

## The models

| Model | For | Direct light | Image-based light |
|---|---|---|---|
| **Default** | most things | GGX, Smith-Schlick, Schlick, Lambert: exactly as before | as before |
| **Clear Coat** | car paint, lacquer, varnish | a second GGX lobe, F0 0.04, Kelemen visibility, over the base attenuated by the coat's Fresnel; the base's F0 seen through the coat and its roughness no less than the coat's [Filament 4.9; Kelemen 2001; Weidlich 2007] | a second specular fetch at the coat's roughness and normal; the screen-space reflection goes to the coat |
| **Anisotropic** | brushed metal, satin | anisotropic GGX and height-correlated anisotropic Smith [Burley 2012; Heitz 2014], Kulla-Conty's roughness split [Kulla 2017] | bent reflection vector [Revie 2011; McAuley 2015; Filament 5.3.4.1] |
| **Cloth** | velvet, cotton, wool | Charlie sheen [Estevez 2017], Neubelt visibility [Neubelt 2013], diffuse wrapped round the terminator tinted by Subsurface Color [Filament 4.12] | prefiltered radiance times the sheen's DG average |
| **Subsurface** | leaves, wax, marble, ice | Default plus transmission through Thickness: forward scattering as a spherical Gaussian, back scattering wrapped [Green 2004; Barre-Brisebois 2011; Filament 4.11] | adds the irradiance from behind, times the back scatter |
| **Skin** | faces, hands | Subsurface's, its diffuse kept apart and blurred in screen space by a skin profile [Jimenez 2015; d'Eon 2007] | the same split |

All of it is in `Engine/Content/Shaders/ShadingModels.glsl` (the lobes,
`EvaluateLightModel`, the custom data) and `IBLShading.glsl`
(`IBL_LightModel`). `Lighting.glsl`'s `ShadeSurfaceSplit` lights a
`LightingSurface` with every light and the ambient in two parts -- the diffuse
light before albedo, and the rest -- which Skin needs apart; `ShadeSurface`
is the Default model's whole light, as before.

## Materials

The Material Output's **Shading Model** (Lit only) and its pins:

| Pin | Default | Read by |
|---|---|---|
| Clear Coat | 1 | Clear Coat: how much clear layer, 0-1 |
| Clear Coat Roughness | 0.05 | Clear Coat |
| Clear Coat Normal | the vertex normal | Clear Coat: world space; smooth over a bumpy base by default |
| Anisotropy | 0 | Anisotropic: -1 to 1; above 0 highlights stretch along the Tangent |
| Tangent | along the UV's U | Anisotropic: world space ([Tangents](#tangents)) |
| Sheen Color | the square root of Base Color | Cloth |
| Subsurface Color | white | Cloth's wrap tint; Subsurface's and Skin's transmitted colour |
| Thickness | 0.5 | Subsurface, Skin: 0 thin (light shines through), 1 thick (none does) |

A pin the model does not read is warned about, as an Unlit material's are;
an unknown model is an error. The `.material` file gains a `ShadingModel`
line for any model but Default (still `NimbleMaterial 3`: a Default material's
file and key are what they were). The pixel permutation key carries the model
in bits 5-7, the templates `MATERIAL_SHADING_MODEL_*` 0/1 defines
(`cook_material_graphs.py`); `MATERIAL_TEMPLATE_VERSION` is 15.

A forward (Translucent Lit) material is shaded by its model at compile time;
a Base Pass one writes its model and custom data into the G-buffer.

## The G-buffer

| Target | Format | R | G | B | A |
|---|---|---|---|---|---|
| GBuffer0 | RGBA8 | Base Color | | | Ambient Occlusion, the screen's folded in |
| GBuffer1 | RGB10A2 | normal, octahedral [Cigolle 2014] | | -- | 0 unlit, 2/3 lit with its ambient baked, 1 lit |
| GBuffer2 | RGBA8 | Metallic | Roughness | Specular | shading model |
| GBuffer3 | RGBA8 | Emissive (unchanged) | | | |
| GBuffer4 | RGBA8 | custom data, per model (below) | | | |
| Velocity | RGBA16F | unchanged, now output 5 | | | |

GBuffer1's two alpha bits keep every reader's tests: `a < 0.5` is unlit,
`a < 0.875` baked. `GDI_RTF_RGB10A2` is new (Vulkan `A2B10G10R10_UNORM_PACK32`,
D3D12 `R10G10B10A2_UNORM`, OpenGL `GL_RGB10_A2`; read back to RGBA8 by
`GDIRgb10A2ToBytes`); Metal has none yet and keeps RGBA8 with the same
encoding. `GDI_MAX_COLOR_TARGETS` is 6.

| Model | R | G | B | A |
|---|---|---|---|---|
| Clear Coat | Clear Coat | Clear Coat Roughness | coat normal, octahedral | |
| Anisotropic | Anisotropy x 0.5 + 0.5 | tangent, octahedral | | |
| Cloth | Sheen Color | | | Subsurface Color's mean |
| Subsurface, Skin | Subsurface Color | | | Thickness |

A deferred Cloth surface's wrap tint is therefore grey; a forward one keeps
its colour.

**Ambient occlusion moved.** The Lighting stage already used all eight
material texture slots (four G-buffer targets, depth, two shadow atlases and
the screen's occlusion). GBuffer4 took the occlusion's: `PPAmbientApply.fsh`
now folds the screen's occlusion into GBuffer0's alpha with a multiplying
blend, right after the AO passes, and the Lighting and IBL passes read one
occlusion. The result is the same product as before.

## Light through a surface

Subsurface and Skin add light from behind (`SM_Transmission`), which the
surface's own shadow does not stop:

- **The sun:** attenuated by the depth its light travels inside whatever is in
  the way, read from the shadow map (`DirectionalShadowDepth`, Shadows.glsl:
  the receiver's depth less the caster's, a little inside the surface, in world
  units by the cascade's depth scale; Jimenez et al. 2010), falling to 1/e
  over `SUBSURFACE_MEAN_FREE_PATH` (1 world unit). A leaf transmits; a head
  behind a wall does not.
- **Point and spot lights:** unshadowed; Thickness alone limits it.

## Skin

Where a Skin material drew in the Base Pass and the scene is HDR
(`[Rendering] SubsurfaceScattering`, on by default):

1. The Lighting stage writes a second target, **Subsurface** (RGBA16F, cleared
   first): a Skin pixel's diffuse light before its albedo, and a 1. The IBL
   pass adds its diffuse there too. Specular and transmission stay in the
   scene colour.
2. **PPSubsurface.fsh** blurs it across into **SubsurfaceBlur**, then down,
   multiplied by the albedo and added to the scene colour. The kernel is
   Jimenez et al.'s separable one, 25 taps from -3 to 3 packed toward the
   centre, weighted by d'Eon and Luebke's sum of Gaussians for skin with
   Jimenez's falloff (1, 0.37, 0.3) and strength (0.48, 0.41, 0.28) per
   channel (`BuildSkinKernel`, RenderContext.cpp). Its width is
   `[Rendering] SubsurfaceWidth` world units (1.2: skin, in centimetres),
   turned into pixels at each pixel's depth. It follows the surface: a tap off
   the skin, or far from its depth, counts as the centre.

Off, or in an 8-bit scene, a Skin pixel lights as Subsurface does.

The Render Target view mode lists both targets.

## Cloth's sheen table

Cloth's image-based sheen needs the Charlie distribution's DG averaged over
the hemisphere for each NdotV and roughness. It lives in the IBL atlas's
spare sixth block, 32 x 32 at (384, 512) (`IBL_SheenDG`, IBLCommon.glsl):
`IBLPrefilter.fsh` writes it once per atlas (block 5), by uniform sampling
(Filament's `DFV_Charlie_Uniform`), so no new texture binding is needed.
Default keeps Karis's analytic split-sum fit.

## Tangents

Meshes carry no tangents yet. An Anisotropic material's unset Tangent comes
from screen derivatives of the world position and UV
(`Material_TangentAlongU`, MaterialCommon.glsl): constant across a triangle,
so a low-poly curved mesh shows its facets in the highlight. Link the Tangent
pin to a smooth direction where it matters -- ShadingModelLab's brushed steel
uses the vertex normal crossed with +Y. A MikkTSpace tangent stream
[Mikkelsen 2008] in `MeshData` and the importers is the fix ([Open](#open)).

## Verification

- **NodeGraph.Tests:** a Default material writes no ShadingModel line and warns
  on Clear Coat's pin; a Clear Coat one names its model and sets its coat; an
  Unlit one has none; an unknown model is an error.
- **RenderCore.Smoke (Null):** the `ShadingModel` line parses, an unknown one is
  refused, the keys (Default unchanged; Clear Coat `0x20`; Masked Skin `0xa1`),
  and each model's name round-trips.
- **RenderCore.Smoke on the GPU** (`NIMBLE_SMOKE_GPU=Vulkan` and `D3D12`): all
  724 checks pass with the new G-buffer. DefaultLit's preview reads 186 187 191
  (it read 186 188 192 before; the octahedral normal). The test materials it
  draws are Games/Sandbox's, which is no longer tracked: recook them from git
  history (93281475^) when the templates change, or their stale shaders write
  the old layout.
- **ShadingModelLab** (WorldsLab; `Tools/ShadingModelLab/make_shadingmodellab.py`):
  a sphere per model and a leaf, backlit by a low sun, in the editor on
  Vulkan. Clear Coat shows a sharp coat over its rough base, Anisotropic a
  stretched band, Velvet a pink rim, Wax and the leaf glow through, and Skin's
  diffuse lands in the Subsurface target alone.

## Open

- **Tangents:** a MikkTSpace stream in `MeshData` (`MTAN`), the glTF and FBX
  importers, and each backend's vertex layouts.
- **White furnace:** a sphere of albedo 1 under a uniform white sky should
  read at most 1 for every model [Kulla 2017]; not yet a check.
- **Cost:** not measured. The Lighting and IBL passes now branch on the model
  per pixel, and read GBuffer4 only where it is not Default; Skin adds two
  full-screen passes that leave non-skin pixels at once. Past a budget,
  classify screen tiles by model and run each tile's own shader
  [El Garawany 2016].
- **Metal:** an RGB10A2 render target.
- Hair, eyes, layered materials, multiple-scattering energy compensation
  [Kulla 2017; Fdez-Aguera 2019].

## References

- Burley 2012. "Physically-Based Shading at Disney." SIGGRAPH course *Practical Physically Based Shading in Film and Game Production*.
- Karis 2013. "Real Shading in Unreal Engine 4." SIGGRAPH PBS course.
- Lagarde, de Rousiers 2014. "Moving Frostbite to Physically Based Rendering 3.0." SIGGRAPH course.
- Heitz 2014. "Understanding the Masking-Shadowing Function in Microfacet-Based BRDFs." *JCGT* 3(2).
- Guy, Agopian. *Physically Based Rendering in Filament*, sections 4.9-4.12 and 5.3. https://google.github.io/filament/Filament.html
- Kelemen, Szirmay-Kalos 2001. "A Microfacet Based Coupled Specular-Matte BRDF Model with Importance Sampling." Eurographics short presentations.
- Weidlich, Wilkie 2007. "Arbitrarily Layered Micro-Facet Surfaces." GRAPHITE.
- Kulla, Conty 2017. "Revisiting Physically Based Shading at Imageworks." SIGGRAPH PBS course.
- Revie 2011. "Implementing Fur Using Deferred Shading." *GPU Pro 2*.
- McAuley 2015. "Rendering the World of Far Cry 4." GDC.
- Estevez, Kulla 2017. "Production Friendly Microfacet Sheen BRDF." Sony Pictures Imageworks.
- Neubelt, Pettineo 2013. "Crafting a Next-Gen Material Pipeline for The Order: 1886." SIGGRAPH PBS course.
- Green 2004. "Real-Time Approximations to Subsurface Scattering." *GPU Gems*, chapter 16.
- Barre-Brisebois, Bouchard 2011. "Approximating Translucency for a Fast, Cheap and Convincing Subsurface Scattering Look." GDC.
- Jimenez, Whelan, Sundstedt, Gutierrez 2010. "Real-Time Realistic Skin Translucency." *IEEE CG&A* 30(4).
- Jimenez et al. 2015. "Separable Subsurface Scattering." *Computer Graphics Forum* 34(6).
- d'Eon, Luebke 2007. "Advanced Techniques for Realistic Real-Time Skin Rendering." *GPU Gems 3*, chapter 14.
- Christensen, Burley 2015. "Approximate Reflectance Profiles for Efficient Subsurface Scattering." Pixar technical memo 15-04 (the alternative profile, not used).
- Mikkelsen 2008. "Simulation of Wrinkled Surfaces Revisited." MSc thesis, University of Copenhagen.
- Schueler 2006. "Normal Mapping without Precomputed Tangents." *ShaderX5*.
- Cigolle et al. 2014. "A Survey of Efficient Representations for Independent Unit Vectors." *JCGT* 3(2).
- El Garawany 2016. "Deferred Lighting in Uncharted 4." SIGGRAPH Advances in Real-Time Rendering.
- Fdez-Aguera 2019. "A Multiple-Scattering Microfacet Model for Real-Time Image-based Lighting." *JCGT* 8(1).
