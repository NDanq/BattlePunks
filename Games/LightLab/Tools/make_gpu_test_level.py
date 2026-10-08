#!/usr/bin/env python3
"""
make_gpu_test_level.py -- writes Content/Levels/GpuTest.nmworld, the GPU test
world (the LightLab module's GpuTest level), as the editor saves a text
world: the manifest and a folder of .nmentity and .nmprefabinst files. Also
writes Content/Prefabs/HerdFox.nmprefab, the herd's fox.

    A town of 4 x 4 blocks on a grid of streets. Each block is a ring of
    brick and plaster buildings -- Static, and marked Occluder, so from the
    street they hide what is inside -- round a courtyard: a cluster of PBR
    props (a metallic / roughness sweep, chrome, dirty chrome, rusted metal)
    or the PBR showcase prefab, and four foxes (HerdFox) the LightLab.Herd
    system walks round it. Half the blocks have a gate in their south side,
    so some courtyards show from the street.

    Lights are the standard kinds, all shadowed: a cascaded sun, Standard
    spot lights over the crossings, a Standard point light in the square,
    the sky (the procedural sky's image-based lighting), and unshadowed
    street lamps. No particle systems.

    The SteerableFox prefab (WASD-free keys: LightLabInput.cpp's Fox map)
    waits in the square at the south end of the main avenue, by the
    Player's start.

The level's benchmark (LightLab.cpp's GpuTest level, [LightLab] Benchmark)
flies the Player along a fixed path through it; see Tools/gpu_test_perf.py.

Entity and instance GUIDs come from their names, so running it again
rewrites the same files. Usage:

    python Games/LightLab/Tools/make_gpu_test_level.py
"""

import hashlib
import math
import xml.etree.ElementTree as ET
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
WORLD    = GAME_DIR / "Content" / "Levels" / "GpuTest.nmworld"
FOLDER   = WORLD.with_suffix("")
PREFABS  = GAME_DIR / "Content" / "Prefabs"

BOX      = "Engine/Content/Meshes/Box.nmmesh"
SPHERE   = "Engine/Content/Meshes/Sphere.nmmesh"
CAPSULE  = "Engine/Content/Meshes/Capsule.nmmesh"
CYLINDER = "Engine/Content/Meshes/Cylinder.nmmesh"

PBR          = "Content/Materials/PBR.material.nmgraph"
BRICKS       = "Content/Materials/Bricks.material.nmgraph"
CHROME       = "Content/Materials/Chrome.material.nmgraph"
DIRTY_CHROME = "Content/Materials/DirtyChrome.material.nmgraph"

STEERABLE_FOX = "Content/Prefabs/SteerableFox.nmprefab"
HERD_FOX      = "Content/Prefabs/HerdFox.nmprefab"
SHOWCASE      = "Content/Prefabs/PBRShowcase.nmprefab"
SKY_TEXTURE   = "Content/Textures/Sky/ProceduralSky.nmtexture"

BLOCKS      = 4       # a side
PITCH       = 2200    # block centre to block centre; streets are PITCH - FOOTPRINT wide
FOOTPRINT   = 1600    # a block's ring of buildings, outside to outside
DEPTH       = 300     # a building, street to courtyard
GATE        = 360     # the gap in a gated block's south side
HERD_RADIUS = 300     # the foxes' circle round the courtyard's middle


def guid(name):
    return hashlib.md5(("GpuTest/" + name).encode()).hexdigest()


def rand(name, low, high):
    """A number in [low, high) from name: the same every run."""
    value = int(hashlib.md5(("GpuTestRand/" + name).encode()).hexdigest()[:8], 16) / 0xFFFFFFFF
    return low + (high - low) * value


def num(value):
    """As the editor writes floats: shortest that round-trips, no trailing .0."""
    text = repr(float(value))
    return text[:-2] if text.endswith(".0") else text


def transform(position, rotation=(0, 0, 0), scale=(1, 1, 1)):
    return ("  <Component Type=\"Transform\" Version=\"1\">\n"
            f"    <Position X=\"{num(position[0])}\" Y=\"{num(position[1])}\" Z=\"{num(position[2])}\" />\n"
            f"    <Rotation X=\"{num(rotation[0])}\" Y=\"{num(rotation[1])}\" Z=\"{num(rotation[2])}\" />\n"
            f"    <Scale X=\"{num(scale[0])}\" Y=\"{num(scale[1])}\" Z=\"{num(scale[2])}\" />\n"
            "  </Component>\n")


def render(mesh, material=PBR, tint=None, metallic=None, roughness=None, casts=True, occluder=False):
    """A Render component (version 5) with one material slot; a PBR slot's
    BaseColor, Metallic and Roughness overridden where given."""
    lines = ["  <Component Type=\"Render\" Version=\"5\">\n", "    <Visible Value=\"true\" />\n"]
    lines += [f"    <Material{slot} Value=\"{material if slot == 0 else ''}\" />\n" for slot in range(8)]
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    params = []
    if tint is not None:
        params.append(("BaseColor", tuple(tint) + (0,)))
    if metallic is not None:
        params.append(("Metallic", (metallic, 0, 0, 0)))
    if roughness is not None:
        params.append(("Roughness", (roughness, 0, 0, 0)))
    lines.append(f"    <ParameterCount Value=\"{len(params)}\" />\n")
    for index, (name, value) in enumerate(params):
        lines.append(f"    <Parameter{index}Name Value=\"{name}\" />\n")
        lines.append(f"    <Parameter{index}Slot Value=\"0\" />\n")
        for axis, component in zip("XYZW", value):
            lines.append(f"    <Parameter{index}{axis} Value=\"{num(component)}\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    lines.append("    <CastInsetShadow Value=\"false\" />\n")
    lines.append(f"    <Occluder Value=\"{'true' if occluder else 'false'}\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def colour(r, g, b):
    return f"R=\"{r}\" G=\"{g}\" B=\"{b}\" A=\"255\""


def shadow_fields(bias, normal_bias, technique=0, tint=(40, 50, 80)):
    return ("    <CastShadows Value=\"true\" />\n"
            f"    <ShadowTechnique Value=\"{technique}\" />\n"
            f"    <ShadowTint {colour(*tint)} />\n"
            f"    <ShadowBias Value=\"{bias}\" />\n"
            f"    <ShadowNormalBias Value=\"{normal_bias}\" />\n")


def spot(rgb, intensity, range_, inner, outer):
    return ("  <Component Type=\"Spot Light\" Version=\"1\">\n"
            f"    <Color {colour(*rgb)} />\n"
            f"    <Intensity Value=\"{intensity}\" />\n"
            f"    <Range Value=\"{range_}\" />\n"
            f"    <InnerConeAngle Value=\"{inner}\" />\n"
            f"    <OuterConeAngle Value=\"{outer}\" />\n"
            "    <Active Value=\"true\" />\n"
            + shadow_fields(0.002, 1.5) +
            "  </Component>\n")


def point(rgb, intensity, radius, casts):
    body = ("  <Component Type=\"Point Light\" Version=\"2\">\n"
            f"    <Color {colour(*rgb)} />\n"
            f"    <Intensity Value=\"{intensity}\" />\n"
            f"    <Radius Value=\"{radius}\" />\n"
            "    <Active Value=\"true\" />\n")
    body += shadow_fields(0.004, 2) if casts else ("    <CastShadows Value=\"false\" />\n"
                                                    "    <ShadowTechnique Value=\"0\" />\n"
                                                    f"    <ShadowTint {colour(40, 50, 80)} />\n"
                                                    "    <ShadowBias Value=\"0.004\" />\n"
                                                    "    <ShadowNormalBias Value=\"2\" />\n")
    return body + "    <SourceRadius Value=\"4\" />\n  </Component>\n"


def entity(name, components, static):
    mobility = " Mobility=\"Static\"" if static else ""
    # Components sorted by type name, as SaveWorldPackage writes them.
    body = "".join(text for _, text in sorted(components))
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            f"<Entity Guid=\"{guid(name)}\" Name=\"{name}\"{mobility}>\n{body}</Entity>\n")


def instance(name, prefab, position, yaw=0.0):
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            f"<Instance Guid=\"{guid(name)}\" Name=\"{name}\" Prefab=\"{prefab}\">\n"
            "  <Entity Id=\"1\">\n"
            "    <Component Type=\"Transform\" Version=\"1\">\n"
            f"      <Position X=\"{num(position[0])}\" Y=\"{num(position[1])}\" Z=\"{num(position[2])}\" />\n"
            f"      <Rotation X=\"0\" Y=\"{num(yaw)}\" Z=\"0\" />\n"
            "    </Component>\n"
            "  </Entity>\n"
            "</Instance>\n")


# --- The herd's fox --------------------------------------------------------

def write_herd_fox():
    """HerdFox.nmprefab: SteerableFox's body and hat socket, and FoxHat's
    pieces under the socket -- the hat its script spawns, here from the
    start -- with no input and no script: LightLab.Herd drives it."""
    steerable = ET.parse(PREFABS / "SteerableFox.nmprefab").getroot()
    hat       = ET.parse(PREFABS / "FoxHat.nmprefab").getroot()
    by_id = {e.get("Id"): e for e in steerable.findall("Entity")}
    hat_by_id = {e.get("Id"): e for e in hat.findall("Entity")}

    out = ET.Element("Prefab", Format="1", Name="HerdFox", NextId="8")
    ET.SubElement(out, "Tick", Mode="Tick")
    root = ET.SubElement(out, "Entity", Id="1", Name="HerdFox")
    root.append(by_id["1"].find("Component[@Type='Transform']"))

    def copy(source, new_id, parent):
        made = ET.SubElement(out, "Entity", Id=new_id, Name=source.get("Name"), Parent=parent)
        for component in source.findall("Component"):
            made.append(component)
        return made

    copy(by_id["2"], "2", "1")          # Body: Animation, Render, Transform
    copy(by_id["3"], "3", "2")          # HatSocket on b_Head_05
    copy(hat_by_id["1"], "4", "3")      # Hat: the hat's root
    for old, new in (("2", "5"), ("3", "6"), ("4", "7")):
        copy(hat_by_id[old], new, "4")  # Brim, Crown, Band

    ET.indent(out, space="  ")
    text = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n" + ET.tostring(out, encoding="unicode") + "\n"
    (PREFABS / "HerdFox.nmprefab").write_text(text, newline="\n")


# --- The town ----------------------------------------------------------------

def main():
    write_herd_fox()
    entities  = []    # (name, text)
    instances = []

    def shape(name, mesh, position, scale, rotation=(0, 0, 0), static=True, **look):
        entities.append((name, entity(name, [("Render", render(mesh, **look)),
                                             ("Transform", transform(position, rotation, scale))], static)))

    # The ground: one slab under the whole town.
    span = BLOCKS * PITCH + 2000
    shape("Ground", BOX, (0, -10, 0), (span, 20, span), tint=(0.16, 0.16, 0.17), roughness=0.92)

    plaster = [(0.78, 0.74, 0.66), (0.62, 0.66, 0.70), (0.80, 0.62, 0.48), (0.55, 0.60, 0.52)]
    half = (BLOCKS - 1) / 2.0
    for bx in range(BLOCKS):
        for bz in range(BLOCKS):
            cx, cz = (bx - half) * PITCH, (bz - half) * PITCH
            block = f"B{bx}{bz}"
            gated = (bx + bz) % 2 == 0

            # --- The ring: two buildings a side, each its own height.
            outer = FOOTPRINT / 2.0
            for side, (nx, nz) in (("S", (0, -1)), ("N", (0, 1)), ("W", (-1, 0)), ("E", (1, 0))):
                for part in (0, 1):
                    name = f"{block}_{side}{part}"
                    height = round(rand(name, 650, 1350) / 10) * 10
                    along = -outer / 2.0 if part == 0 else outer / 2.0
                    length = FOOTPRINT / 2.0
                    if side in ("W", "E"):
                        length -= DEPTH                     # the corners belong to the north and south sides
                        along = along * (FOOTPRINT / 2.0 - DEPTH) / (FOOTPRINT / 2.0)
                    if gated and side == "S":
                        length -= GATE / 2.0                # leave the gate between the two
                        along += (-GATE / 4.0) if part == 0 else (GATE / 4.0)
                    middle = outer - DEPTH / 2.0
                    if side in ("S", "N"):
                        position = (cx + along, height / 2.0, cz + nz * middle)
                        scale    = (length, height, DEPTH)
                    else:
                        position = (cx + nx * middle, height / 2.0, cz + along)
                        scale    = (DEPTH, height, length)
                    if rand(name + "/brick", 0, 1) < 0.5:
                        shape(name, BOX, position, scale, material=BRICKS, occluder=True)
                    else:
                        tint = plaster[int(rand(name + "/tint", 0, len(plaster)))]
                        shape(name, BOX, position, scale, tint=tint, roughness=0.85, occluder=True)

            # --- The courtyard.
            if (bx, bz) in ((1, 1), (2, 2)):
                instances.append((f"{block}_Showcase", instance(f"{block}_Showcase", SHOWCASE, (cx, 0, cz + 120))))
            else:
                # A 3 x 3 sweep: metallic across, roughness back.
                for i in range(3):
                    for j in range(3):
                        mesh = (SPHERE, CAPSULE, CYLINDER)[(i + j + bx + bz) % 3]
                        tint = plaster[(i + 2 * j + bx) % len(plaster)]
                        name = f"{block}_Prop{i}{j}"
                        shape(name, mesh, (cx - 160 + 160 * i, 60, cz + 40 + 140 * j), (90, 120 if mesh != SPHERE else 90, 90),
                              tint=tint, metallic=i / 2.0, roughness=0.15 + 0.35 * j)
                shape(f"{block}_Chrome", SPHERE, (cx - 300, 80, cz - 220), (150, 150, 150), material=CHROME)
                shape(f"{block}_DirtyChrome", CAPSULE, (cx + 300, 100, cz - 220), (110, 200, 110), material=DIRTY_CHROME)
                shape(f"{block}_Plinth", BOX, (cx, 15, cz + 180), (520, 30, 420), tint=(0.3, 0.3, 0.32), roughness=0.7)

            # --- Its herd: four foxes round the middle, each its own way round.
            for index in range(4):
                angle = 90.0 * index + 45.0
                x = cx + HERD_RADIUS * math.cos(math.radians(angle))
                z = cz + HERD_RADIUS * math.sin(math.radians(angle))
                name = f"HerdFox_{block}_{index}"
                instances.append((name, instance(name, HERD_FOX, (x, 0, z), yaw=rand(name, 0, 360))))

    # --- The square at the south end of the main avenue, and its fox.
    south = -(half * PITCH + FOOTPRINT / 2.0) - 900
    shape("SquarePlinth", BOX, (0, 10, south), (900, 20, 900), tint=(0.42, 0.40, 0.38), roughness=0.6)
    shape("SquareStatue", CAPSULE, (0, 260, south + 250), (140, 220, 140), material=DIRTY_CHROME)
    instances.append(("SteerableFox", instance("SteerableFox", STEERABLE_FOX, (180, 0, south - 120), yaw=0)))

    # --- Lights. The sun: cascaded, low enough that the buildings throw long shadows.
    sun = ("  <Component Type=\"Directional Light\" Version=\"2\">\n"
           f"    <Color {colour(255, 240, 220)} />\n"
           "    <Intensity Value=\"5\" />\n"
           "    <Active Value=\"true\" />\n"
           + shadow_fields(0.0015, 2, technique=2) +
           "    <ShadowDistance Value=\"6000\" />\n"
           "    <CascadeCount Value=\"4\" />\n"
           "    <CascadeSplitLambda Value=\"0.75\" />\n"
           "  </Component>\n")
    entities.append(("Sun", entity("Sun", [("Directional Light", sun), ("Transform", transform((0, 3000, 0), (42, 32, 0)))], True)))

    sky = ("  <Component Type=\"Sky Light\" Version=\"2\">\n"
           f"    <SkyColor {colour(125, 150, 190)} />\n"
           f"    <GroundColor {colour(70, 62, 55)} />\n"
           "    <Intensity Value=\"0.35\" />\n"
           "    <Active Value=\"true\" />\n"
           f"    <Environment Value=\"{SKY_TEXTURE}\" />\n"
           "    <EnvironmentIntensity Value=\"0.45\" />\n"
           "    <EnvironmentRotation Value=\"0\" />\n"
           "    <DrawBackground Value=\"true\" />\n"
           "  </Component>\n")
    entities.append(("Sky", entity("Sky", [("Sky Light", sky), ("Transform", transform((0, 0, 0)))], True)))

    # Standard-shadowed spots over the four inner crossings, pointing (nearly) down.
    for index, (x, z) in enumerate(((-PITCH / 2, -PITCH / 2), (PITCH / 2, -PITCH / 2), (-PITCH / 2, PITCH / 2), (PITCH / 2, PITCH / 2))):
        name = f"CrossingSpot{index}"
        entities.append((name, entity(name, [("Spot Light", spot((255, 225, 190), 18, 2200, 22, 34)),
                                             ("Transform", transform((x, 1400, z), (85, 0, 0)))], True)))
    # A Standard-shadowed point light over the square's statue.
    entities.append(("SquareLamp", entity("SquareLamp", [("Point Light", point((255, 200, 150), 10, 900, True)),
                                                         ("Transform", transform((0, 420, south - 50)))], True)))
    # Unshadowed lamps down the main avenue.
    for index in range(BLOCKS + 1):
        z = (index - BLOCKS / 2.0) * PITCH
        for sx in (-1, 1):
            name = f"AvenueLamp{index}{'L' if sx < 0 else 'R'}"
            entities.append((name, entity(name, [("Point Light", point((255, 210, 160), 3, 500, False)),
                                                 ("Transform", transform((sx * 240, 300, z)))], True)))

    # --- The player, in the square, looking up the avenue.
    camera = ("  <Component Type=\"Camera\" Version=\"2\">\n"
              "    <ZNear Value=\"5\" />\n"
              "    <ZFar Value=\"30000\" />\n"
              "    <Active Value=\"true\" />\n"
              "    <Projection Value=\"1\" />\n"
              "    <FovY Value=\"70\" />\n"
              "  </Component>\n")
    player_input = ("  <Component Type=\"Input\" Version=\"1\">\n"
                    "    <Map Value=\"Default\" />\n"
                    "    <PlayerIndex Value=\"0\" />\n"
                    "    <Enabled Value=\"true\" />\n"
                    "  </Component>\n")
    entities.append(("Player", entity("Player", [("Camera", camera), ("Input", player_input),
                                                 ("Transform", transform((0, 260, south - 700), (8, 0, 0)))], False)))

    post = ("  <Component Type=\"Post Process\" Version=\"2\">\n"
            "    <Stack Value=\"Content/PostProcess/Global.pp.nmgraph\" />\n"
            "    <Priority Value=\"0\" />\n"
            "    <Enabled Value=\"true\" />\n"
            "    <BlendDistance Value=\"0\" />\n"
            "  </Component>\n")
    entities.append(("PostProcessGlobal", entity("PostProcessGlobal", [("Post Process", post), ("Transform", transform((0, 0, 0)))], False)))

    # --- The files.
    FOLDER.mkdir(parents=True, exist_ok=True)
    for stale in list(FOLDER.glob("*.nmentity")) + list(FOLDER.glob("*.nmprefabinst")):
        stale.unlink()
    manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n"]
    for name, text in entities:
        (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
        manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
    for name, text in instances:
        (FOLDER / f"{guid(name)}.nmprefabinst").write_text(text, newline="\n")
        manifest.append(f"  <Instance Guid=\"{guid(name)}\" />\n")
    manifest.append("</World>\n")
    WORLD.write_text("".join(manifest), newline="\n")
    print(f"{WORLD}: {len(entities)} entities, {len(instances)} prefab instances; {PREFABS / 'HerdFox.nmprefab'}")


if __name__ == "__main__":
    main()
