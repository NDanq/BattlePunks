#!/usr/bin/env python3
"""
make_baked_lighting_level.py -- writes Content/Levels/BakedLighting.nmworld,
baked lighting's demo (the LightLab module's BakedLighting level), as the
editor saves a text world: the manifest and a folder of .nmentity files.

    A Cornell-style room, 8 m square and 5 m high, open to the front: a red
    wall on the left, a green one on the right, white floor, ceiling and back
    wall, and a window in the back wall. Inside, a tall box, a short box and a
    capsule. All of that is Static, and so are its lights: a sun through the
    window, a warm lamp under the ceiling and the sky. A ball and a capsule
    are Dynamic: lit by the light probes, the ball circling (LightLab.Swing).
    So is a brass lattice cage hung in the sun's beam (InsetLattice, from
    make_inset_lattice_mesh.py): inset shadows' demo, its grid shadow falling
    in the window's patch of sun on the floor.

The world has baked lighting on (its <Settings>). Bake it after running this:

    NimbleEditor Content/Levels/BakedLighting.nmworld, then Build > Bake Lightmaps, then Save

or headless, from the binaries folder:

    QT_QPA_PLATFORM=offscreen ./NimbleEditor --rhi 0 <world> \\
        --command "lighting bake" --command save --command quit

Entity GUIDs come from their names, so running it again rewrites the same
files (a bake made since still matches). Usage:

    python Games/LightLab/Tools/make_baked_lighting_level.py
"""

import hashlib
from pathlib import Path

GAME_DIR = Path(__file__).resolve().parent.parent
WORLD    = GAME_DIR / "Content" / "Levels" / "BakedLighting.nmworld"
FOLDER   = WORLD.with_suffix("")

BOX     = "Engine/Content/Meshes/Box.nmmesh"
SPHERE  = "Engine/Content/Meshes/Sphere.nmmesh"
CAPSULE = "Engine/Content/Meshes/Capsule.nmmesh"
LATTICE = "Content/Meshes/InsetLattice.nmmesh"
PBR     = "Content/Materials/PBR.material.nmgraph"

WHITE = (0.85, 0.85, 0.85)
RED   = (0.85, 0.12, 0.10)
GREEN = (0.15, 0.75, 0.20)


def guid(name):
    return hashlib.md5(("BakedLighting/" + name).encode()).hexdigest()


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


def render(mesh, colour, roughness=0.6, casts=True):
    """A mesh drawn with LightLab's PBR material, its BaseColor overridden --
    which the bake reads as the surface's albedo."""
    return render_slots(mesh, [(colour, 0, roughness)], casts)


def render_slots(mesh, slots, casts=True, inset=False):
    """render() with a PBR slot per (colour, metallic, roughness): section i
    draws with slot i, its parameters overridden in that slot. inset: Cast
    Inset Shadow on (the component's version 4; without it, version 3 as
    before)."""
    lines = [f"  <Component Type=\"Render\" Version=\"{4 if inset else 3}\">\n", "    <Visible Value=\"true\" />\n"]
    lines += [f"    <Material{slot} Value=\"{PBR if slot < len(slots) else ''}\" />\n" for slot in range(8)]
    lines.append(f"    <Mesh Value=\"{mesh}\" />\n")
    params = [(slot, name, value) for slot, (tint, metallic, roughness) in enumerate(slots)
              for name, value in (("BaseColor", tint + (0,)), ("Metallic", (metallic, 0, 0, 0)), ("Roughness", (roughness, 0, 0, 0)))]
    lines.append(f"    <ParameterCount Value=\"{len(params)}\" />\n")
    for index, (slot, name, value) in enumerate(params):
        lines.append(f"    <Parameter{index}Name Value=\"{name}\" />\n")
        lines.append(f"    <Parameter{index}Slot Value=\"{slot}\" />\n")
        for axis, component in zip("XYZW", value):
            lines.append(f"    <Parameter{index}{axis} Value=\"{num(component)}\" />\n")
    lines.append(f"    <CastShadows Value=\"{'true' if casts else 'false'}\" />\n")
    if inset:
        lines.append("    <CastInsetShadow Value=\"true\" />\n")
    lines.append("  </Component>\n")
    return "".join(lines)


def colour(r, g, b):
    return f"R=\"{r}\" G=\"{g}\" B=\"{b}\" A=\"255\""


def entity(name, components, static):
    mobility = " Mobility=\"Static\"" if static else ""
    # Components sorted by type name, as SaveWorldPackage writes them.
    body = "".join(text for _, text in sorted(components))
    return ("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
            f"<Entity Guid=\"{guid(name)}\" Name=\"{name}\"{mobility}>\n{body}</Entity>\n")


def main():
    entities = []

    def shape(name, mesh, position, scale, tint, rotation=(0, 0, 0), static=True, roughness=0.6):
        entities.append((name, entity(name, [("Render", render(mesh, tint, roughness)),
                                             ("Transform", transform(position, rotation, scale))], static)))

    # --- The room: boxes, so every wall has an inside and an outside.
    shape("Floor",       BOX, (0, -10, 0),     (840, 20, 840), WHITE)
    shape("Ceiling",     BOX, (0, 510, 0),     (840, 20, 840), WHITE)
    shape("WallLeft",    BOX, (-410, 250, 0),  (20, 500, 800), RED)
    shape("WallRight",   BOX, (410, 250, 0),   (20, 500, 800), GREEN)
    # The back wall, round a window 240 wide, 200-380 high.
    shape("BackLower",   BOX, (0, 100, 410),   (800, 200, 20), WHITE)
    shape("BackUpper",   BOX, (0, 440, 410),   (800, 120, 20), WHITE)
    shape("BackLeft",    BOX, (-260, 290, 410), (280, 180, 20), WHITE)
    shape("BackRight",   BOX, (260, 290, 410), (280, 180, 20), WHITE)

    # --- In it.
    shape("TallBox",     BOX, (-150, 150, 130), (120, 300, 120), WHITE, rotation=(0, 20, 0))
    shape("ShortBox",    BOX, (170, 60, -40),  (130, 120, 130), WHITE, rotation=(0, -15, 0))
    shape("Pillar",      CAPSULE, (150, 100, 230), (90, 100, 90), (0.9, 0.55, 0.2), roughness=0.4)

    # --- Dynamic: lit by the probes. Swinger circles (LightLab.Swing).
    shape("Swinger",     SPHERE, (-40, 110, -120), (80, 80, 80), WHITE, static=False, roughness=0.3)
    shape("Visitor",     CAPSULE, (-280, 90, -230), (60, 90, 60), (0.3, 0.45, 0.9), static=False, roughness=0.4)

    # --- Dynamic: inset shadows' demo. A brass cage round a red core, in the
    # sun's beam between the window and the floor; its grid shadow lands in
    # the window's patch of sun, from its own map (Cast Inset Shadow): the
    # baked sun casts nothing else from a dynamic entity. Slot 0 the lattice,
    # slot 1 the core.
    lattice = render_slots(LATTICE, [((0.9, 0.62, 0.22), 1, 0.35), ((0.8, 0.1, 0.08), 0, 0.4)], inset=True)
    entities.append(("InsetLattice", entity("InsetLattice", [("Render", lattice),
                                                             ("Transform", transform((10, 150, 170), (15, 30, 0), (90, 90, 90)))], False)))

    # --- Static lights.
    sun = ("  <Component Type=\"Directional Light\" Version=\"2\">\n"
           f"    <Color {colour(255, 236, 205)} />\n"
           "    <Intensity Value=\"5\" />\n"
           "    <Active Value=\"true\" />\n"
           "    <CastShadows Value=\"true\" />\n"
           "    <ShadowTechnique Value=\"2\" />\n"
           f"    <ShadowTint {colour(40, 50, 80)} />\n"
           "    <ShadowBias Value=\"0.0015\" />\n"
           "    <ShadowNormalBias Value=\"2\" />\n"
           "    <ShadowDistance Value=\"3000\" />\n"
           "    <CascadeCount Value=\"4\" />\n"
           "    <CascadeSplitLambda Value=\"0.75\" />\n"
           "  </Component>\n")
    # Down and toward the viewer (-Z): through the window onto the floor.
    entities.append(("Sun", entity("Sun", [("Directional Light", sun), ("Transform", transform((0, 800, 800), (32, 190, 0)))], True)))

    lamp = ("  <Component Type=\"Point Light\" Version=\"1\">\n"
            f"    <Color {colour(255, 196, 140)} />\n"
            "    <Intensity Value=\"8\" />\n"
            "    <Radius Value=\"750\" />\n"
            "    <Active Value=\"true\" />\n"
            "    <CastShadows Value=\"true\" />\n"
            "    <ShadowTechnique Value=\"0\" />\n"
            f"    <ShadowTint {colour(110, 135, 200)} />\n"
            "    <ShadowBias Value=\"0.004\" />\n"
            "    <ShadowNormalBias Value=\"2\" />\n"
            "  </Component>\n")
    entities.append(("Lamp", entity("Lamp", [("Point Light", lamp), ("Transform", transform((0, 440, -60)))], True)))

    sky = ("  <Component Type=\"Sky Light\" Version=\"2\">\n"
           f"    <SkyColor {colour(120, 140, 175)} />\n"
           f"    <GroundColor {colour(70, 60, 50)} />\n"
           "    <Intensity Value=\"0.5\" />\n"
           "    <Active Value=\"true\" />\n"
           "    <Environment Value=\"\" />\n"
           "    <EnvironmentIntensity Value=\"1\" />\n"
           "    <EnvironmentRotation Value=\"0\" />\n"
           "    <DrawBackground Value=\"true\" />\n"
           "  </Component>\n")
    entities.append(("Sky", entity("Sky", [("Sky Light", sky), ("Transform", transform((0, 0, 0)))], True)))

    # --- The player, outside the open front, looking in.
    camera = ("  <Component Type=\"Camera\" Version=\"2\">\n"
              "    <ZNear Value=\"5\" />\n"
              "    <ZFar Value=\"20000\" />\n"
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
                                                 ("Transform", transform((0, 260, -1050), (6, 0, 0)))], False)))

    post = ("  <Component Type=\"Post Process\" Version=\"2\">\n"
            "    <Stack Value=\"Content/PostProcess/Global.pp.nmgraph\" />\n"
            "    <Priority Value=\"0\" />\n"
            "    <Enabled Value=\"true\" />\n"
            "    <BlendDistance Value=\"0\" />\n"
            "  </Component>\n")
    entities.append(("PostProcessGlobal", entity("PostProcessGlobal", [("Post Process", post), ("Transform", transform((0, 0, 0)))], False)))

    # --- The files.
    FOLDER.mkdir(parents=True, exist_ok=True)
    for stale in FOLDER.glob("*.nmentity"):
        stale.unlink()
    manifest = ["<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n", "<World Format=\"1\">\n",
                "  <Settings>\n",
                "    <Lighting BakedLighting=\"true\" TexelsPerUnit=\"0.3\" Bounces=\"2\" SamplesPerTexel=\"128\" ProbeSpacing=\"100\" />\n",
                "  </Settings>\n"]
    for name, text in entities:
        (FOLDER / f"{guid(name)}.nmentity").write_text(text, newline="\n")
        manifest.append(f"  <Entity Guid=\"{guid(name)}\" />\n")
    manifest.append("</World>\n")
    WORLD.write_text("".join(manifest), newline="\n")
    print(f"{WORLD}: {len(entities)} entities")


if __name__ == "__main__":
    main()
