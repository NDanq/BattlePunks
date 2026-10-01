#!/usr/bin/env python3
"""
cook_material_graphs.py -- check or cook the shaders material graphs generate.

A material graph (*.material.nmgraph) compiles to a .fsh in the engine's macro
GLSL, next to the graph. This script puts those files -- and the engine's own
shaders in Engine/Content/Shaders -- through GDI's preprocessor and tool
lookup, by importing GDI's cook_shaders.py read-only (Modules/GDI is a
vendored subtree). Every #include resolves against Engine/Content/Shaders, not
GDI's Shaders/, and no .qrc is written: the output is plain files the
runtime's file provider reads.

Usage:
    python cook_material_graphs.py -check Glow.fsh [More.fsh ...]
        Compile each to SPIR-V in a temporary directory (glslc). Prints each
        failure's compiler output; exits 1 if any failed. The editor runs this
        after saving a material graph.

    python cook_material_graphs.py -target <OpenGL|Vulkan|D3D12> -src PATH [-src PATH ...] -out DIR [-subdir NAME]
        Cook each shader PATH, or every .vsh/.fsh under a directory PATH
        (recursively), into DIR/<Platform>/[NAME/], named as GDI names its own:
            OpenGL  <name>          preprocessed GLSL
            Vulkan  <name>.spv      SPIR-V
            D3D12   <name>.cso      DXBC (via spirv-cross and fxc)

        The renderer loads a material "Glow" from Shaders/Cooked/<Platform>/
        Materials/Glow.fsh[.spv|.cso] -- so the editor, and a game's build
        (Engine/cmake/NimbleMaterials.cmake), cook with -subdir Materials into
        the runtime directory's Shaders/Cooked. The engine's build cooks
        Engine/Content/Shaders the same way, without -subdir.
"""

import argparse
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR         = Path(__file__).parent.resolve()
GDI_SCRIPTS_DIR    = SCRIPT_DIR.parent.parent / "GDI" / "Scripts"
ENGINE_SHADERS_DIR = SCRIPT_DIR.parent.parent.parent / "Content" / "Shaders"


def _import_cook_shaders(scripts_dir):
    # No __pycache__ inside GDI's tree: it is a vendored subtree, and an
    # import would otherwise leave compiled bytecode beside cook_shaders.py.
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(scripts_dir))
    try:
        import cook_shaders
    except ImportError as exc:
        print(f"ERROR: cannot import GDI's cook_shaders.py from {scripts_dir}: {exc}", file=sys.stderr)
        sys.exit(2)

    # Includes resolve against the engine's Content/Shaders. Every call (here
    # and inside _compile_spirv) passes the module-global SHADERS_SRC as the
    # include root, so rebinding it reroutes them all.
    cook_shaders.SHADERS_SRC = ENGINE_SHADERS_DIR
    return cook_shaders


def _sources(dirs, cs):
    found = []
    for directory in dirs:
        root = Path(directory)
        if root.is_file() and root.suffix in cs.EXT_TO_TYPE:
            found.append(root.resolve())
            continue
        if not root.is_dir():
            print(f"ERROR: -src {directory} is neither a shader nor a directory", file=sys.stderr)
            sys.exit(2)
        found += [f for f in root.rglob("*") if f.suffix in cs.EXT_TO_TYPE]
    return sorted(found)


def _spirv(cs, glslc, src, spv_path, extra_defines=None):
    """cook_shaders._compile_spirv, which exits on failure -- turned into a bool
    so every file is reported, not just the first."""
    try:
        cs._compile_spirv(glslc, src, spv_path, extra_defines)
        return True
    except SystemExit:
        return False


def check(cs, files):
    glslc = cs._find_glslc()
    failures = 0
    with tempfile.TemporaryDirectory() as tmp_dir:
        for name in files:
            src = Path(name).resolve()
            if src.suffix not in cs.EXT_TO_TYPE:
                print(f"ERROR: {name} is not a .vsh/.fsh/.csh shader", file=sys.stderr)
                failures += 1
                continue
            if _spirv(cs, glslc, src, Path(tmp_dir) / (src.name + ".spv")):
                print(f"  ok  {src.name}")
            else:
                failures += 1
    return failures


def cook(cs, target, src_dirs, out_root, subdir=""):
    out_dir = Path(out_root) / target / subdir if subdir else Path(out_root) / target
    out_dir.mkdir(parents=True, exist_ok=True)
    sources = _sources(src_dirs, cs)
    print(f"[{target}] {len(sources)} graph shader(s) -> {out_dir}")

    failures = 0
    if target == "OpenGL":
        for src in sources:
            shader_type = cs.EXT_TO_TYPE[src.suffix]
            processed = cs.preprocess(src, shader_type, cs.SHADERS_SRC, {shader_type: "1", "WITH_OPENGL": "1"})
            (out_dir / src.name).write_text(processed, encoding="utf-8")
            print(f"  {src.name}")
        return 0

    glslc = cs._find_glslc()
    if target == "Vulkan":
        for src in sources:
            if _spirv(cs, glslc, src, out_dir / (src.name + ".spv")):
                print(f"  {src.name} -> {src.name}.spv")
            else:
                failures += 1
        return failures

    # D3D12: GLSL -> SPIR-V -> HLSL -> DXBC, as cook_shaders.cook_d3d12.
    spirv_cross = cs._find_spirv_cross()
    fxc         = cs._find_fxc()
    with tempfile.TemporaryDirectory() as tmp_dir:
        for src in sources:
            is_vertex = cs.EXT_TO_TYPE[src.suffix] == cs.VERTEX_SHADER
            spv_path  = Path(tmp_dir) / (src.name + ".spv")
            hlsl_path = Path(tmp_dir) / (src.name + ".hlsl")
            if not _spirv(cs, glslc, src, spv_path):
                failures += 1
                continue

            cross_args = [spirv_cross, "--hlsl", "--shader-model", "50"]
            if is_vertex:
                cross_args += ["--flip-vert-y", "--hlsl-flatten-matrix-vertex-input-semantics"]
            cross_args += [str(spv_path), "--output", str(hlsl_path)]
            fxc_args = [fxc, "/nologo", "/T", "vs_5_0" if is_vertex else "ps_5_0",
                        "/E", "main", "/O3", "/Fo", str(out_dir / (src.name + ".cso")), str(hlsl_path)]

            for args, tool in ((cross_args, "spirv-cross"), (fxc_args, "fxc")):
                result = subprocess.run(args, capture_output=True, text=True)
                if result.returncode != 0:
                    print(f"ERROR: {tool} failed for {src.name}:\n{result.stdout}{result.stderr}", file=sys.stderr)
                    failures += 1
                    break
            else:
                print(f"  {src.name} -> {src.name}.cso")
    return failures


def main():
    parser = argparse.ArgumentParser(description="Check or cook the shaders material graphs generate.")
    parser.add_argument("-check", nargs="+", metavar="FILE", help="compile these to SPIR-V and report errors")
    parser.add_argument("-target", choices=("OpenGL", "Vulkan", "D3D12"), help="cook for this platform")
    parser.add_argument("-src", action="append", metavar="DIR", help="a shader, or a directory of shaders (repeatable)")
    parser.add_argument("-out", metavar="DIR", help="cooked output root; <Platform>/ is created below it")
    parser.add_argument("-subdir", default="", metavar="NAME", help="a folder below <Platform>/ to cook into (the runtime uses Materials)")
    parser.add_argument("-gdi-scripts", default=str(GDI_SCRIPTS_DIR), metavar="DIR",
                        help="where GDI's cook_shaders.py lives (default: beside this module)")
    args = parser.parse_args()

    cs = _import_cook_shaders(args.gdi_scripts)

    if args.check:
        sys.exit(1 if check(cs, args.check) else 0)
    if args.target and args.src and args.out:
        sys.exit(1 if cook(cs, args.target, args.src, args.out, args.subdir) else 0)

    parser.error("give -check FILE..., or -target with -src and -out")


if __name__ == "__main__":
    main()
