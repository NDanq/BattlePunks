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
    weather    surface variation over a set (graphs.weathering): grime, streak (r, g, b,
               amount), rust (the streak replaces the colour and is not metal), edge
               ("polish" / "wear"), detail (Detail_Normal strength), top (moss on upward
               faces: texture path, metres, amount). Opaque sets only.
    custom     a graph of its own (graphs.CUSTOM[custom]) instead of the kit graph

Vertex colour, as kitlib.py writes it: RGB ambient occlusion baked per vertex (it darkens
base colour a little and multiplies the AO), A how convex the vertex is (edge wear).
"""

KIT_TEX = "Content/Textures/Kit"
KIT_MAT = "Content/Materials/Kit"

MATS = {}


def mat(name, **kw):
    MATS[name] = dict(name=name, **kw)


# Weathering presets (graphs.weathering).
W_BRASS = dict(grime=0.4, streak=(0.30, 0.47, 0.38, 0.3), rust=True, edge="polish", detail=0.5)
W_BRASS_CLEAN = dict(grime=0.15, edge="polish", detail=0.35)
W_COPPER = dict(grime=0.4, streak=(0.27, 0.50, 0.41, 0.5), rust=True, edge="polish", detail=0.5)
W_STEEL = dict(grime=0.5, streak=(0.36, 0.17, 0.07, 0.7), rust=True, edge="polish", detail=0.6)
W_PAINT = dict(grime=0.55, streak=(0.38, 0.18, 0.07, 0.55), rust=True, detail=0.4)
W_STONE = dict(grime=0.6, streak=(0.52, 0.50, 0.47, 1.0), edge="wear", detail=0.3)
W_WOOD = dict(grime=0.5, streak=(0.62, 0.64, 0.52, 0.8), edge="wear", detail=0.3)
W_WALNUT = dict(grime=0.15, edge="wear")
W_ROCK = dict(grime=0.4, streak=(0.62, 0.6, 0.56, 0.7), detail=0.4, top=("Content/Textures/Terrain/Grass", 2.0, 1.4))

# Metals
mat("BrassBrushed", set="Brass", metres=0.5, model="Anisotropic", anisotropy=0.75, rough_mul=0.85,
    weather=W_BRASS_CLEAN)
mat("BrassAged", set="Brass", metres=0.5, rough_mul=1.1, weather=W_BRASS)
mat("Copper", set="Copper", metres=1.0, weather=W_COPPER)
mat("SteelDark", set="Steel", metres=1.0, tint=(0.55, 0.55, 0.58), weather=W_STEEL)
mat("SteelBrushed", set="Steel", metres=1.0, model="Anisotropic", anisotropy=0.8, tint=(1.4, 1.4, 1.4),
    rough_mul=0.8, weather=W_BRASS_CLEAN)
mat("PlateSteel", set="Steel", metres=0.5, model="Anisotropic", anisotropy=0.7, tint=(1.15, 1.15, 1.17),
    rough_mul=0.9, weather=dict(W_STEEL, grime_metres=1.0))
mat("PanelGreen", set="RivetPanel", metres=2.0, paint=(0.18, 0.32, 0.28), weather=W_PAINT)
mat("PanelRed", set="RivetPanel", metres=2.0, paint=(0.42, 0.11, 0.07), weather=W_PAINT)
mat("PanelCream", set="RivetPanel", metres=2.0, paint=(0.80, 0.74, 0.60), weather=W_PAINT)
mat("Grating", set="Grating", metres=0.5, domain="Masked", two_sided=True)
# Wood and stone
mat("Walnut", set="Walnut", metres=1.0, model="ClearCoat", clearcoat=1.0, coat_rough=0.06, weather=W_WALNUT)
mat("Planks", set="Planks", metres=2.0, weather=W_WOOD)
mat("WoodBeam", set="Planks", metres=2.0, tint=(0.62, 0.56, 0.5), weather=W_WOOD)
mat("StoneBlocks", set="StoneBlocks", metres=3.0, weather=W_STONE)
mat("Rock", set="Rock", metres=4.0, weather=W_ROCK)
mat("Cliff", set="CliffStrata", metres=6.0, weather=dict(W_ROCK, streak=(0.5, 0.48, 0.45, 1.0)))
mat("Concrete", set="Concrete", metres=2.0, weather=W_STONE)
mat("MarbleFloor", set="MarbleFloor", metres=2.0, weather=dict(grime=0.25))
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
mat("Steam", base=(0.92, 0.93, 0.95), domain="Translucent", model="Volume", opacity=0.7, phase=0.45, wisp=(0.35, 0.09))
mat("Rubber", base=(0.035, 0.035, 0.035), rough=0.78)
mat("PaintRed", base=(0.55, 0.05, 0.03), rough=0.38, metal=0.0)
mat("PaintBlack", base=(0.03, 0.03, 0.03), rough=0.35, metal=0.0)
mat("Canvas", set="Canvas", metres=4.0, model="Cloth", sheen=(0.45, 0.43, 0.38), sss=(0.7, 0.65, 0.5),
    weather=dict(grime=0.3, streak=(0.72, 0.7, 0.64, 0.6)))
# The sea (graphs.sea_material / sea_foam_material)
mat("Sea", custom="sea", base=(0.03, 0.12, 0.13), rough=0.05, domain="Translucent", opacity=0.85)
mat("SeaFoam", custom="sea_foam", base=(0.9, 0.92, 0.92), rough=0.6, domain="Translucent", opacity=0.6)
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
