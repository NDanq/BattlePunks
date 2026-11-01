#!/usr/bin/env python3
"""
make_particles.py -- Demo Lab's particle effects: Content/Particles/*.nmparticle (text,
read as staged, no cook) and their materials, Content/Materials/Particles/*.material.nmgraph
(graphs.particle_material; NodeGraphGenerate fills their code, build_all.py's "generate").

    python Games/DemoLab/Tools/Art/make_particles.py

Systems (RENDERPIPELINE.md "Particles"; positions and sizes in centimetres, the entity's
+Y is the emitter's up -- turn the entity to aim a cone):

    SteamVent      L1 vents, L3 pressure release: a slow column that drifts downwind, and
                   fast wisps at its root
    SteamJet       a leaking pipe joint: hissing bursts along +Y every 2.5 s
    Sparks         an electrical fault or grinding: bursts of hot sparks that fall, bounce on
                   the floor (Collision 0 = the entity's height: place it on the floor or
                   lower CollisionHeight) and a flash
    ReactorArcs    L3 reactor: four jittering lightning beams from the dome down to the insulator
                   terminals (the entity at ReactorCore + (0, 1300, 0) cm; or set Beam End /
                   the component's Beam Target), with spark showers
    SprinklerSpray L2 alarm: a cone of droplets down from a ceiling head (point the entity's +Y
                   down: rotate 180 about X), killed 450 cm below, and a mist
    DustMotes      calm rooms: motes drifting in a 6 x 4 x 6 m box (in light shafts they catch
                   the sun)
"""

import sys
from pathlib import Path

ART = Path(__file__).resolve().parent
sys.path.insert(0, str(ART / "lib"))
import graphs        # noqa: E402

GAME = ART.parents[1]
MAT_DIR = GAME / "Content" / "Materials" / "Particles"
SYS_DIR = GAME / "Content" / "Particles"
TEX = "Content/Textures/Kit"
MAT = "Content/Materials/Particles"

DEFAULTS = [
    ("Enabled", "true"), ("Type", "Sprite"), ("Modules", "Shape Velocity Forces"),
    ("Mesh", "Engine/Content/Meshes/SpriteOctagon.nmmesh"), ("Material", ""), ("Offset", "0,0,0"),
    ("Billboard", "FaceCamera"), ("VelocityStretch", "0"), ("MeshOrientation", "RandomSpin"),
    ("RandomRotation", "true"), ("RotationRate", "0.3"), ("SpawnMode", "Continuous"), ("MaxParticles", "256"),
    ("Shape", "Sphere"), ("ShapeSurface", "false"), ("BoxSize", "100,100,100"), ("ConeAngle", "25"),
    ("RingWidth", "0"), ("ShapeSpeed", "0"), ("SpawnRate", "20"), ("BurstCount", "16"), ("BurstInterval", "0"),
    ("BurstCycles", "1"), ("Lifetime", "2"), ("LifetimeVariance", "0.3"), ("EmitterRadius", "10"), ("Seed", "0"),
    ("CullDistance", "8000"), ("PauseOffscreen", "true"), ("Velocity", "0,0,0"), ("VelocitySpread", "0"),
    ("Acceleration", "0,0,0"), ("Drag", "0"), ("Turbulence", "0"), ("CollisionResponse", "None"),
    ("CollisionHeight", "-100000"), ("Bounce", "0.3"), ("Friction", "0.2"), ("FlipbookColumns", "1"),
    ("FlipbookRows", "1"), ("FlipbookFrames", "0"), ("FlipbookRate", "0"), ("FlipbookCycles", "1"),
    ("FlipbookRandomStart", "false"), ("BeamEnd", "0,200,0"), ("BeamSegments", "16"), ("BeamNoise", "0"),
    ("BeamNoiseSpeed", "4"), ("SizeMode", "StartEnd"), ("StartSize", "10"), ("EndSize", "10"), ("SizeKeys", ""),
    ("ColorMode", "StartEnd"), ("StartColor", "255,255,255,255"), ("EndColor", "255,255,255,0"),
    ("ColorCurve", "0.6"), ("FadeIn", "0.1"), ("FadeOut", "0.5"), ("ColorKeys", ""),
]


def emitter(name, **kw):
    keys = dict(DEFAULTS)
    for k, v in kw.items():
        if k not in keys:
            raise KeyError(f"{name}: unknown key {k}")
        keys[k] = v if isinstance(v, str) else (",".join(str(x) for x in v) if isinstance(v, tuple) else str(v).lower()
                                               if isinstance(v, bool) else str(v))
    lines = [f"Emitter {name}"] + [f"    {k} {keys[k]}".rstrip() for k, _ in DEFAULTS]
    return "\n".join(lines)


def write_system(name, emitters):
    SYS_DIR.mkdir(parents=True, exist_ok=True)
    path = SYS_DIR / f"{name}.nmparticle"
    path.write_text("NimbleParticleSystem 1\n\n" + "\n\n".join(emitters) + "\n", newline="\n")
    print(f"  {path.relative_to(GAME)}: {len(emitters)} emitters")


def m(kind):
    return f"{MAT}/P_{kind}.material.nmgraph"


def systems():
    write_system("SteamVent", [
        emitter("Column", Material=m("Steam"), MaxParticles=320, SpawnRate=38, Lifetime=4.2, LifetimeVariance=0.35,
                EmitterRadius=16, Velocity=(0, 190, 0), VelocitySpread=22, Acceleration=(28, 12, 6), Drag=0.55,
                Turbulence=55, StartSize=28, EndSize=300, StartColor="226,226,224,80", EndColor="205,205,205,0",
                FadeIn=0.08, FadeOut=0.65, RotationRate=0.25),
        emitter("Wisps", Material=m("Steam"), MaxParticles=96, SpawnRate=24, Lifetime=0.9, LifetimeVariance=0.4,
                EmitterRadius=8, Velocity=(0, 420, 0), VelocitySpread=30, Drag=1.4, Turbulence=30, StartSize=12,
                EndSize=70, StartColor="240,240,240,110", EndColor="230,230,230,0", FadeIn=0.05, FadeOut=0.5),
    ])
    write_system("SteamJet", [
        emitter("Jet", Material=m("Steam"), SpawnMode="Burst", BurstCount=60, BurstInterval=2.5, BurstCycles=0,
                MaxParticles=192, Shape="Cone", ConeAngle=7, ShapeSpeed=950, EmitterRadius=2, Lifetime=1.3,
                LifetimeVariance=0.4, Drag=2.2, Turbulence=40, Acceleration=(0, 60, 0), StartSize=6, EndSize=110,
                StartColor="240,240,240,120", EndColor="220,220,220,0", FadeIn=0.02, FadeOut=0.6),
    ])
    write_system("Sparks", [
        emitter("Sparks", Material=m("Spark"), Modules="Shape Velocity Forces Collision", SpawnMode="Burst",
                BurstCount=36, BurstInterval=1.6, BurstCycles=0, MaxParticles=160, Shape="Cone", ConeAngle=70,
                ShapeSpeed=520, EmitterRadius=1, Lifetime=0.9, LifetimeVariance=0.5, VelocitySpread=80,
                Acceleration=(0, -980, 0), Drag=0.5, Billboard="VelocityAligned", VelocityStretch=0.025,
                CollisionResponse="Bounce", CollisionHeight=0, Bounce=0.35, Friction=0.3, StartSize=2.0,
                EndSize=0.6, StartColor="255,240,190,255", EndColor="255,90,20,0", ColorCurve=0.5, FadeIn=0,
                FadeOut=0.4, RandomRotation=False),
        emitter("Flash", Material=m("Spark"), SpawnMode="Burst", BurstCount=2, BurstInterval=1.6, BurstCycles=0,
                MaxParticles=8, Modules="Shape", EmitterRadius=1, Lifetime=0.09, LifetimeVariance=0.2, StartSize=40,
                EndSize=70, StartColor="255,220,170,255", EndColor="255,150,60,0", FadeIn=0, FadeOut=0.7),
    ])
    arcs = []
    # From ReactorCore's dome (place the entity at (0, 1300, 0) cm of the reactor) down to four
    # of its insulator terminals (radius 135 cm, 1255 cm up).
    import math
    for i, (k, seed) in enumerate(((0, 1), (1, 2), (3, 3), (4, 4))):
        a = math.radians(60 * k)
        end = (round(135 * math.cos(a)), -45, round(135 * math.sin(a)))
        arcs.append(emitter(f"Arc{i}", Type="Beam", Modules="", Material=m("Arc"), BeamEnd=end, BeamSegments=20,
                            BeamNoise=22, BeamNoiseSpeed=14 + 3 * i, Seed=seed, Lifetime=0.4, StartSize=5,
                            EndSize=3, StartColor="170,215,255,255", EndColor="120,180,255,200", FadeIn=0,
                            FadeOut=0, CullDistance=12000))
    arcs.append(emitter("ArcSparks", Material=m("Spark"), SpawnMode="Burst", BurstCount=14, BurstInterval=0.7,
                        BurstCycles=0, MaxParticles=96, Shape="Sphere", ShapeSpeed=380, EmitterRadius=20,
                        Lifetime=0.6, LifetimeVariance=0.5, VelocitySpread=120, Acceleration=(0, -500, 0), Drag=1.0,
                        Billboard="VelocityAligned", VelocityStretch=0.02, StartSize=1.6, EndSize=0.4,
                        StartColor="200,230,255,255", EndColor="90,150,255,0", FadeIn=0, FadeOut=0.5,
                        RandomRotation=False))
    write_system("ReactorArcs", arcs)
    write_system("SprinklerSpray", [
        emitter("Drops", Material=m("Spray"), Modules="Shape Velocity Forces Collision", MaxParticles=1024,
                SpawnRate=420, Shape="Cone", ConeAngle=58, ShapeSpeed=330, EmitterRadius=3, Lifetime=1.4,
                LifetimeVariance=0.25, VelocitySpread=25, Acceleration=(0, -980, 0), Drag=0.25,
                Billboard="VelocityAligned", VelocityStretch=0.012, CollisionResponse="Kill", CollisionHeight=-450,
                StartSize=1.4, EndSize=1.0, StartColor="210,225,235,200", EndColor="200,215,230,150", FadeIn=0.02,
                FadeOut=0.15, RandomRotation=False),
        emitter("Mist", Material=m("Steam"), MaxParticles=128, SpawnRate=14, Shape="Cone", ConeAngle=60,
                ShapeSpeed=90, EmitterRadius=10, Lifetime=3.0, LifetimeVariance=0.3, Acceleration=(0, -60, 0),
                Drag=0.8, Turbulence=25, StartSize=40, EndSize=220, StartColor="225,232,238,26",
                EndColor="225,232,238,0", FadeIn=0.2, FadeOut=0.6),
    ])
    write_system("DustMotes", [
        emitter("Motes", Material=m("Dust"), MaxParticles=200, SpawnRate=16, Shape="Box", BoxSize=(600, 400, 600),
                Lifetime=12, LifetimeVariance=0.3, Velocity=(0, 2, 0), VelocitySpread=60, Turbulence=6, Drag=0.2,
                StartSize=0.9, EndSize=0.9, StartColor="255,244,220,120", EndColor="255,244,220,0", FadeIn=0.25,
                FadeOut=0.3, CullDistance=4000),
        emitter("Haze", Material=m("Dust"), MaxParticles=24, SpawnRate=1.5, Shape="Box", BoxSize=(600, 400, 600),
                Lifetime=14, LifetimeVariance=0.2, Velocity=(0, 1, 0), VelocitySpread=40, Turbulence=3,
                StartSize=6, EndSize=8, StartColor="255,240,215,40", EndColor="255,240,215,0", FadeIn=0.3,
                FadeOut=0.4, CullDistance=4000),
    ])


def materials():
    written = []
    for kind in ("Steam", "Dust", "Spray", "Spark", "Arc"):
        g = graphs.particle_material(f"P_{kind}", kind, TEX)
        path = MAT_DIR / f"P_{kind}.material.nmgraph"
        g.write(path)
        written.append(path)
        print(f"  {path.relative_to(GAME)}")
    return written


def main():
    materials()
    systems()


if __name__ == "__main__":
    main()
