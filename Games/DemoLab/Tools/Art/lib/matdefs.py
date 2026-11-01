"""
matdefs.py -- every kit material Demo Lab's meshes name, as data: make_materials.py turns
each into a .material.nmgraph, and the Blender scripts into a Principled BSDF preview of
the same textures (so the contact sheets show what the engine will draw, roughly).

A definition's keys (all optional but name):

    set        a texture set in Content/Textures/Kit (BaseColor, Normal, ORM), sampled at
               UV0 x 1 / metres (the kit meshes' UV0 is in metres)
    tex        a single BaseColor texture sampled at UV0 (0-1 decals: unit=True)
    base       a constant base colour (no set), rough, metal
    tint       multiplies the set's base colour (a Tint parameter)
    paint      RivetPanel's paint colour (BaseColor A is where paint is; convex edges --
               vertex colour A -- wear through to steel)
    rough_mul  multiplies the set's roughness
    model      the shading model: Default, ClearCoat, Anisotropic, Cloth, Subsurface, Volume
    clearcoat, coat_rough, anisotropy, sheen, sss, thickness: that model's pins
    domain     Opaque, Masked (opacity from BaseColor A), Translucent
    opacity, refraction  Translucent's
    emissive   (colour, intensity): an Emissive Color and Emissive Intensity parameter;
               pulse: a Pulse Speed parameter (the glow breathes)
    two_sided

Vertex colour, as kitlib.py writes it: RGB ambient occlusion baked per vertex (it darkens
base colour a little and multiplies the AO), A how convex the vertex is (edge wear).
"""

KIT_TEX = "Content/Textures/Kit"
KIT_MAT = "Content/Materials/Kit"

MATS = {}


def mat(name, **kw):
    MATS[name] = dict(name=name, **kw)


# Metals
mat("BrassBrushed", set="Brass", metres=0.5, model="Anisotropic", anisotropy=0.75, rough_mul=0.85)
mat("BrassAged", set="Brass", metres=0.5, rough_mul=1.1)
mat("Copper", set="Copper", metres=1.0)
mat("SteelDark", set="Steel", metres=1.0, tint=(0.55, 0.55, 0.58))
mat("SteelBrushed", set="Steel", metres=1.0, model="Anisotropic", anisotropy=0.8, tint=(1.4, 1.4, 1.4),
    rough_mul=0.8)
mat("PanelGreen", set="RivetPanel", metres=2.0, paint=(0.18, 0.32, 0.28))
mat("PanelRed", set="RivetPanel", metres=2.0, paint=(0.42, 0.11, 0.07))
mat("PanelCream", set="RivetPanel", metres=2.0, paint=(0.80, 0.74, 0.60))
mat("Grating", set="Grating", metres=0.5, domain="Masked", two_sided=True)
# Wood and stone
mat("Walnut", set="Walnut", metres=1.0, model="ClearCoat", clearcoat=1.0, coat_rough=0.06)
mat("Planks", set="Planks", metres=2.0)
mat("WoodBeam", set="Planks", metres=2.0, tint=(0.62, 0.56, 0.5))
mat("StoneBlocks", set="StoneBlocks", metres=3.0)
mat("Rock", set="Rock", metres=4.0)
mat("Concrete", set="Concrete", metres=2.0)
mat("MarbleFloor", set="MarbleFloor", metres=2.0)
# Shading-model samples (and their everyday uses)
mat("Marble", set="Marble", metres=1.0, model="Subsurface", sss=(0.95, 0.88, 0.78), thickness=0.6)
mat("Leather", set="Leather", metres=0.25, model="Cloth", sheen=(0.32, 0.22, 0.15), sss=(0.45, 0.2, 0.1))
mat("Velvet", set="Velvet", metres=0.1, model="Cloth", sheen=(0.95, 0.42, 0.55), sss=(0.6, 0.05, 0.15))
mat("Enamel", base=(0.02, 0.20, 0.13), rough=0.4, metal=0.35, model="ClearCoat", clearcoat=1.0, coat_rough=0.03)
mat("Wax", base=(0.93, 0.84, 0.62), rough=0.42, model="Subsurface", sss=(1.0, 0.55, 0.2), thickness=0.3)
mat("LeafPanel", tex="LeafPanel_BaseColor", unit=True, rough=0.55, model="Subsurface", sss=(1.4, 1.5, 0.9),
    thickness=0.12, two_sided=True)
mat("GlassFrosted", set="Glass", metres=1.0, base=(0.85, 0.88, 0.9), domain="Translucent", opacity=0.6,
    refraction=1.04, rough_add=0.35)
mat("GlassClear", set="Glass", metres=1.0, base=(0.6, 0.65, 0.66), domain="Translucent", opacity=0.12,
    refraction=1.02, rough_mul=0.3)
mat("Steam", base=(0.92, 0.93, 0.95), domain="Translucent", model="Volume", opacity=0.35, phase=0.3)
mat("Rubber", base=(0.035, 0.035, 0.035), rough=0.78)
mat("PaintRed", base=(0.55, 0.05, 0.03), rough=0.38, metal=0.0)
mat("PaintBlack", base=(0.03, 0.03, 0.03), rough=0.35, metal=0.0)
mat("Canvas", set="Canvas", metres=4.0, model="Cloth", sheen=(0.45, 0.43, 0.38), sss=(0.7, 0.65, 0.5))
# Decals
mat("GaugeFace", tex="GaugeFace_BaseColor", unit=True, rough=0.2, emissive=((1.0, 0.93, 0.78), 0.35))
mat("Books", tex="Books_BaseColor", unit=True, rough=0.6)
# Light sources (Emissive Color x Emissive Intensity, per entity)
mat("LampGlow", base=(1.0, 0.9, 0.75), rough=0.3, emissive=((1.0, 0.62, 0.3), 12.0))
mat("ReactorGlow", base=(0.2, 0.5, 0.6), rough=0.2, emissive=((0.35, 0.8, 1.0), 16.0), pulse=0.8)
mat("IntelGlow", base=(0.1, 0.3, 0.15), rough=0.1, emissive=((0.35, 1.0, 0.5), 6.0), pulse=1.6)
mat("BeaconLight", base=(0.6, 0.15, 0.05), rough=0.25, emissive=((1.0, 0.3, 0.06), 14.0), pulse=3.0)


def material_path(name):
    return f"{KIT_MAT}/{name}.material.nmgraph"
