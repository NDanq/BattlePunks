#!/usr/bin/env python3
"""
cook_material_graphs.py -- check or cook the engine's shaders and the
material functions material graphs generate (RenderCore/MATERIALS.md).

A material graph (*.material.nmgraph) compiles to <Name>.mtl.glsl -- a
material function -- and <Name>.material, both stored in the graph file's
<gen_code> (NodeGraph's README). The engine's templates
(Engine/Content/Shaders/MaterialTemplate_VS/PS.glsl) include the function as
<Material.glsl>; this script compiles each template once per permutation the
material's <gen_code> needs. It reuses GDI's preprocessor and tool lookup
by importing GDI's cook_shaders.py read-only (Modules/GDI is a vendored
subtree). Every #include resolves against Engine/Content/Shaders, and no .qrc
is written: the output is plain files, or one shader cache.

Usage:
    python cook_material_graphs.py -check FILE [FILE ...]
        Compile each to SPIR-V in a temporary directory (glslc): a .vsh/.fsh
        as is, a .mtl.glsl in every permutation it needs. Prints each
        failure's compiler output; exits 1 if any failed.

    python cook_material_graphs.py -target <OpenGL|Vulkan|D3D12> -src PATH [-src PATH ...]
                                   (-out DIR [-data-out DIR] | -cache FILE -module NAME)
                                   [-root DIR] [-subdir NAME] [-defaults]
        Cook every .vsh/.fsh, .mtl.glsl and .material.nmgraph given, or under
        a directory PATH (recursively), into DIR/<Platform>/, named as GDI
        names its shaders:
            OpenGL  <name>          preprocessed GLSL
            Vulkan  <name>.spv      SPIR-V
            D3D12   <name>.cso      DXBC (via spirv-cross and fxc)
        A material's permutations are named <Name>@<key>.vsh/.fsh, the key
        8 hex digits (MaterialPermutation.h). -defaults adds the shared
        vertex shaders, MaterialDefault@<key>.vsh.

        A material graph is named by its path relative to -root, as the
        runtime names it: Materials/Glow.material.nmgraph cooks to
        <Platform>/Materials/Glow@<key>.* and its description to
        Materials/Glow.material -- under -data-out when loose. A graph with no
        <gen_code> fails: open and save it in the editor. Loose .mtl.glsl
        files (each with its <Name>.material beside it; the editor's live
        cook) go below -subdir instead.

        -cache writes one shader cache instead (Shaders/Cache/<Module>.<Platform>.nmsc):
        every cooked file at its runtime path under Shaders/Cooked/, plus each
        material's .material at Materials/<Name>.material. RenderCore mounts
        it as a read-only file layer (ShaderCache.h).

The renderer loads the material "Materials/Glow.material.nmgraph" from
Materials/Glow.material and
Shaders/Cooked/<Platform>/Materials/Glow@<key>.<stage>[.spv|.cso]
(Engine/cmake/NimbleMaterials.cmake cooks a game's graphs with -root at the
game's folder).
"""

import argparse
import hashlib
import xml.etree.ElementTree as ElementTree
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR         = Path(__file__).parent.resolve()
GDI_SCRIPTS_DIR    = SCRIPT_DIR.parent.parent / "GDI" / "Scripts"
ENGINE_SHADERS_DIR = SCRIPT_DIR.parent.parent.parent / "Content" / "Shaders"

TEMPLATE_VS      = ENGINE_SHADERS_DIR / "MaterialTemplate_VS.glsl"
TEMPLATE_PS      = ENGINE_SHADERS_DIR / "MaterialTemplate_PS.glsl"
DEFAULT_MATERIAL = ENGINE_SHADERS_DIR / "MaterialDefault.mtl.glsl"
MATERIAL_INCLUDE = "Material.glsl"   # what the templates include; mapped per material

# --- Permutation keys (RenderCore's MaterialPermutation.h says the same) ------
DOMAINS    = ["Opaque", "Masked", "Translucent", "PostProcess"]   # bits 0-2
SHADINGS   = ["Lit", "Unlit"]                                     # bits 3-4
VERTEX_INS = ["Quad", "Mesh", "Fullscreen"]                       # bits 8-10

CACHE_VERSION = 1


def pixel_key(domain, shading):
    return DOMAINS.index(domain) | (SHADINGS.index(shading) << 3)


def vertex_key(vertex_in):
    return VERTEX_INS.index(vertex_in) << 8


def permutation_name(name, key, stage):
    return f"{name}@{key:08x}.{'vsh' if stage == 'vertex' else 'fsh'}"


def pixel_defines(domain, shading):
    defines = {f"MATERIAL_DOMAIN_{d.upper()}": "1" if d == domain else "0" for d in DOMAINS}
    defines.update({f"MATERIAL_SHADING_{s.upper()}": "1" if s == shading else "0" for s in SHADINGS})
    return defines


def vertex_defines(vertex_in, has_offset):
    defines = {f"MATERIAL_VERTEX_{v.upper()}": "1" if v == vertex_in else "0" for v in VERTEX_INS}
    defines["MATERIAL_HAS_OFFSET"] = "1" if has_offset else "0"
    return defines


def template_version():
    text = (ENGINE_SHADERS_DIR / "MaterialCommon.glsl").read_text(encoding="utf-8")
    found = re.search(r"#\s*define\s+MATERIAL_TEMPLATE_VERSION\s+(\d+)", text)
    return int(found.group(1)) if found else 0


# --- GDI's toolchain, rerouted -------------------------------------------------

# The material function the templates' <Material.glsl> means right now.
_current_material = [DEFAULT_MATERIAL]


def _import_cook_shaders(scripts_dir):
    # No __pycache__ inside GDI's tree: it is a vendored subtree.
    sys.dont_write_bytecode = True
    sys.path.insert(0, str(scripts_dir))
    try:
        import cook_shaders
    except ImportError as exc:
        print(f"ERROR: cannot import GDI's cook_shaders.py from {scripts_dir}: {exc}", file=sys.stderr)
        sys.exit(2)

    # Includes resolve against the engine's Content/Shaders: every call passes
    # the module-global SHADERS_SRC as the include root, so rebinding it
    # reroutes them all. preprocess() recurses through its module-global name,
    # so wrapping it maps <Material.glsl> on every target.
    cook_shaders.SHADERS_SRC = ENGINE_SHADERS_DIR
    gdi_preprocess = cook_shaders.preprocess

    def preprocess(file_path, *args, **kwargs):
        if Path(file_path).name == MATERIAL_INCLUDE:
            file_path = _current_material[0]
        return gdi_preprocess(file_path, *args, **kwargs)

    cook_shaders.preprocess = preprocess
    return cook_shaders


class Job:
    """One shader to cook: its source, stage, defines and output name."""

    def __init__(self, source, stage, out_name, defines=None, material=None):
        self.source   = Path(source)
        self.stage    = stage                 # "vertex" or "pixel"
        self.out_name = out_name              # e.g. "Glow@00000000.fsh"
        self.defines  = defines or {}
        self.material = material              # the .mtl.glsl <Material.glsl> means, if any

    def label(self):
        return self.out_name if not self.material else f"{self.material.name} -> {self.out_name}"


def _preprocess(cs, job, platform_define):
    shader_type = cs.VERTEX_SHADER if job.stage == "vertex" else cs.PIXEL_SHADER
    defines = {shader_type: "1", platform_define: "1", **job.defines}
    _current_material[0] = job.material or DEFAULT_MATERIAL
    return cs.preprocess(job.source, shader_type, ENGINE_SHADERS_DIR, defines)


def _spirv(cs, glslc, job, spv_path):
    """GLSL -> SPIR-V; the stage from a temporary file's extension, as glslc reads it."""
    try:
        processed = _preprocess(cs, job, "WITH_VULKAN")
    except SystemExit:
        return False

    fd, tmp_path = tempfile.mkstemp(suffix=".vert" if job.stage == "vertex" else ".frag")
    try:
        with os.fdopen(fd, "w", encoding="utf-8") as tmp:
            tmp.write(processed)
        result = subprocess.run([glslc, tmp_path, "-o", str(spv_path)], capture_output=True, text=True)
    finally:
        if os.path.exists(tmp_path):
            os.unlink(tmp_path)

    if result.returncode != 0:
        print(f"ERROR: glslc failed for {job.label()}:\n{result.stderr}", file=sys.stderr)
        return False
    return True


def cook_job(cs, tools, target, job, out_dir):
    """Cooks one job into out_dir; False (reported) on failure."""
    if target == "OpenGL":
        try:
            text = _preprocess(cs, job, "WITH_OPENGL")
        except SystemExit:
            return False
        (out_dir / job.out_name).write_text(text, encoding="utf-8")
        return True

    if target == "Vulkan":
        return _spirv(cs, tools["glslc"], job, out_dir / (job.out_name + ".spv"))

    # D3D12: GLSL -> SPIR-V -> HLSL -> DXBC, as cook_shaders.cook_d3d12.
    with tempfile.TemporaryDirectory() as tmp_dir:
        spv_path  = Path(tmp_dir) / "shader.spv"
        hlsl_path = Path(tmp_dir) / "shader.hlsl"
        if not _spirv(cs, tools["glslc"], job, spv_path):
            return False

        is_vertex  = job.stage == "vertex"
        cross_args = [tools["spirv-cross"], "--hlsl", "--shader-model", "50"]
        if is_vertex:
            cross_args += ["--hlsl-flatten-matrix-vertex-input-semantics"]   # no Y flip: GDI.h
        cross_args += [str(spv_path), "--output", str(hlsl_path)]
        fxc_args = [tools["fxc"], "/nologo", "/T", "vs_5_0" if is_vertex else "ps_5_0",
                    "/E", "main", "/O3", "/Fo", str(out_dir / (job.out_name + ".cso")), str(hlsl_path)]

        for args, tool in ((cross_args, "spirv-cross"), (fxc_args, "fxc")):
            result = subprocess.run(args, capture_output=True, text=True)
            if result.returncode != 0:
                print(f"ERROR: {tool} failed for {job.label()}:\n{result.stdout}{result.stderr}", file=sys.stderr)
                return False
    return True


def find_tools(cs, target):
    tools = {}
    if target in ("Vulkan", "D3D12"):
        tools["glslc"] = cs._find_glslc()
    if target == "D3D12":
        tools["spirv-cross"] = cs._find_spirv_cross()
        tools["fxc"] = cs._find_fxc()
    return tools


# --- Materials ------------------------------------------------------------------

def read_material(mtl_path):
    """<Name>.material beside <Name>.mtl.glsl: its Domain, Shading and HasOffset."""
    name = mtl_path.name[:-len(".mtl.glsl")]
    data_path = mtl_path.with_name(name + ".material")
    meta = {"Name": name, "Domain": "Opaque", "Shading": "Lit", "HasOffset": "0", "Path": data_path}
    if not data_path.is_file():
        print(f"ERROR: {mtl_path.name} has no {data_path.name} beside it", file=sys.stderr)
        return None

    for line in data_path.read_text(encoding="utf-8").splitlines():
        words = line.split("#", 1)[0].split()
        if len(words) >= 2 and words[0] in ("Domain", "Shading", "HasOffset"):
            meta[words[0]] = words[1]
    if meta["Domain"] not in DOMAINS or meta["Shading"] not in SHADINGS:
        print(f"ERROR: {data_path.name}: unknown Domain '{meta['Domain']}' or Shading '{meta['Shading']}'", file=sys.stderr)
        return None
    return meta


def material_jobs(mtl_path, prefix=""):
    """A material's permutations: its own Domain and Shading for the pixel
    stage; with World Position Offset, its own vertex stage per vertex input
    it can be drawn with (MATERIALS.md, "Permutations"). prefix: the folders
    its files go below ("Materials/")."""
    meta = read_material(mtl_path)
    if meta is None:
        return None, None

    name, domain, shading = prefix + meta["Name"], meta["Domain"], meta["Shading"]
    meta["Runtime"] = name + ".material"
    key = pixel_key(domain, shading)
    jobs = [Job(TEMPLATE_PS, "pixel", permutation_name(name, key, "pixel"), pixel_defines(domain, shading), mtl_path)]
    if meta["HasOffset"] == "1" and domain != "PostProcess":
        for vertex_in in ("Quad", "Mesh"):
            jobs.append(Job(TEMPLATE_VS, "vertex", permutation_name(name, vertex_key(vertex_in), "vertex"),
                            vertex_defines(vertex_in, True), mtl_path))
    return jobs, meta


def default_jobs():
    """The shared vertex shaders every material without an offset draws with."""
    return [Job(TEMPLATE_VS, "vertex", permutation_name("MaterialDefault", vertex_key(v), "vertex"),
                vertex_defines(v, False), DEFAULT_MATERIAL) for v in VERTEX_INS]


GRAPH_SUFFIX = ".material.nmgraph"


def extract_graph(graph_path, root, work_dir):
    """A material graph's <gen_code> as <Name>.mtl.glsl and <Name>.material in
    work_dir/<folders>/; (mtl path, "<folders>/") or (None, None), reported."""
    try:
        relative = graph_path.resolve().relative_to(Path(root).resolve())
    except ValueError:
        print(f"ERROR: {graph_path} is not below -root {root}", file=sys.stderr)
        return None, None
    try:
        tree = ElementTree.parse(graph_path)
    except ElementTree.ParseError as exc:
        print(f"ERROR: {graph_path}: not valid XML: {exc}", file=sys.stderr)
        return None, None

    outputs = {}
    for output in tree.getroot().findall("gen_code/Output"):
        outputs[output.get("Extension", "")] = output.text or ""
    if ".mtl.glsl" not in outputs or ".material" not in outputs:
        print(f"ERROR: {graph_path} has no generated code (<gen_code>) -- open it in the editor and save it",
              file=sys.stderr)
        return None, None

    name   = relative.name[:-len(GRAPH_SUFFIX)]
    folder = relative.parent.as_posix()
    prefix = "" if folder == "." else folder + "/"
    out    = Path(work_dir) / folder
    out.mkdir(parents=True, exist_ok=True)
    (out / (name + ".mtl.glsl")).write_text(outputs[".mtl.glsl"], encoding="utf-8", newline="\n")
    (out / (name + ".material")).write_text(outputs[".material"], encoding="utf-8", newline="\n")
    return out / (name + ".mtl.glsl"), prefix


def collect(paths):
    """(plain shaders, material functions, material graphs) under each path."""
    plain, materials, graphs = [], [], []
    for given in paths:
        root = Path(given)
        files = [root] if root.is_file() else sorted(root.rglob("*")) if root.is_dir() else None
        if files is None:
            print(f"ERROR: -src {given} is neither a file nor a directory", file=sys.stderr)
            sys.exit(2)
        for f in files:
            if f.name.endswith(GRAPH_SUFFIX):
                graphs.append(f.resolve())
            elif f.name.endswith(".mtl.glsl") and f.name != DEFAULT_MATERIAL.name:
                materials.append(f.resolve())
            elif f.suffix in (".vsh", ".fsh"):
                plain.append(f.resolve())
    return plain, materials, graphs


def plain_job(src):
    return Job(src, "vertex" if src.suffix == ".vsh" else "pixel", src.name)


# --- The shader cache --------------------------------------------------------------

def _chunk(tag, version, payload):
    tag_value = (ord(tag[0]) << 24) | (ord(tag[1]) << 16) | (ord(tag[2]) << 8) | ord(tag[3])
    return struct.pack("<IHI", tag_value, version, len(payload)) + payload


def _string(text):
    data = text.encode("utf-8")
    return struct.pack("<I", len(data)) + data


def fnv1a64(data):
    h = 0xcbf29ce484222325
    for byte in data:
        h ^= byte
        h = (h * 0x100000001b3) & 0xFFFFFFFFFFFFFFFF
    return h


def write_cache(cache_path, module, target, files):
    """files: {runtime path: bytes}. 'NMSC' header, a 'BLOB' per distinct
    content, a 'FILE' per path naming its blob by hash (ShaderCache.cpp)."""
    out = bytearray()
    out += _chunk("NMSC", CACHE_VERSION, _string(module) + _string(target) + struct.pack("<I", template_version()))

    blobs = {}
    for data in files.values():
        blobs.setdefault(fnv1a64(data), data)
    for h in sorted(blobs):
        out += _chunk("BLOB", 1, struct.pack("<QI", h, len(blobs[h])) + blobs[h])
    for path in sorted(files):
        out += _chunk("FILE", 1, _string(path) + struct.pack("<Q", fnv1a64(files[path])))

    cache_path = Path(cache_path)
    cache_path.parent.mkdir(parents=True, exist_ok=True)
    cache_path.write_bytes(bytes(out))
    print(f"[{target}] {module}: {len(files)} file(s), {len(blobs)} distinct, {len(out)} bytes -> {cache_path}")


# --- Commands ------------------------------------------------------------------------

def check(cs, files, root):
    glslc = cs._find_glslc()
    failures = 0
    plain, materials, graphs = collect(files)
    jobs = [plain_job(p) for p in plain]

    with tempfile.TemporaryDirectory() as tmp_dir:
        for graph in graphs:
            mtl, _ = extract_graph(graph, root or graph.parent, Path(tmp_dir) / "graphs")
            if mtl is None:
                failures += 1
            else:
                materials.append(mtl)
        for mtl in materials:
            mjobs, _ = material_jobs(mtl)
            if mjobs is None:
                failures += 1
                continue
            jobs += mjobs

        for job in jobs:
            if _spirv(cs, glslc, job, Path(tmp_dir) / "check.spv"):
                print(f"  ok  {job.label()}")
            else:
                failures += 1
    return failures


def cook(cs, target, src_paths, out_root=None, subdir="", defaults=False, cache=None, module="", root=None, data_out=None):
    plain, materials, graphs = collect(src_paths)
    if graphs and not root:
        print("ERROR: material graphs need -root (what their runtime paths are relative to)", file=sys.stderr)
        return 1

    with tempfile.TemporaryDirectory() as work_dir, tempfile.TemporaryDirectory() as tmp_dir:
        # Loose functions go below -subdir; graphs below their own folders.
        sub_prefix = subdir.strip("/") + "/" if subdir else ""
        sources = [(mtl, sub_prefix) for mtl in materials]
        failures = 0
        for graph in graphs:
            mtl, prefix = extract_graph(graph, root, work_dir)
            if mtl is None:
                failures += 1
            else:
                sources.append((mtl, prefix))

        jobs, metas = [plain_job(p) for p in plain], []
        for job in jobs:
            job.out_name = sub_prefix + job.out_name
        for mtl, prefix in sources:
            mjobs, meta = material_jobs(mtl, prefix)
            if mjobs is None:
                failures += 1
                continue
            jobs += mjobs
            metas.append(meta)
        if defaults:
            for job in default_jobs():
                job.out_name = sub_prefix + job.out_name
                jobs.append(job)

        tools = find_tools(cs, target)
        out_dir = Path(tmp_dir) / target if cache else Path(out_root) / target
        out_dir.mkdir(parents=True, exist_ok=True)

        print(f"[{target}] {len(jobs)} shader(s) -> {cache or out_dir}")
        for job in jobs:
            (out_dir / job.out_name).parent.mkdir(parents=True, exist_ok=True)
            if cook_job(cs, tools, target, job, out_dir):
                print(f"  {job.label()}")
            else:
                failures += 1

        if cache and not failures:
            files = {}
            for f in sorted(Path(tmp_dir).rglob("*")):
                if f.is_file():
                    files["Shaders/Cooked/" + f.relative_to(tmp_dir).as_posix()] = f.read_bytes()
            for meta in metas:
                files[meta["Runtime"]] = meta["Path"].read_bytes()
            write_cache(cache, module, target, files)
        elif data_out and not failures:
            for meta in metas:
                destination = Path(data_out) / meta["Runtime"]
                destination.parent.mkdir(parents=True, exist_ok=True)
                data = meta["Path"].read_bytes()
                if not destination.is_file() or destination.read_bytes() != data:
                    destination.write_bytes(data)
    return failures


def main():
    parser = argparse.ArgumentParser(description="Check or cook engine shaders and material functions.")
    parser.add_argument("-check", nargs="+", metavar="FILE", help="compile these to SPIR-V and report errors")
    parser.add_argument("-target", choices=("OpenGL", "Vulkan", "D3D12"), help="cook for this platform")
    parser.add_argument("-src", action="append", metavar="PATH", help="a shader or material function, or a directory of them (repeatable)")
    parser.add_argument("-out", metavar="DIR", help="cooked output root; <Platform>/ is created below it")
    parser.add_argument("-subdir", default="", metavar="NAME", help="a folder below <Platform>/ for loose .mtl.glsl files and -defaults (the runtime uses Materials)")
    parser.add_argument("-root", metavar="DIR", help="what material graphs' runtime paths are relative to (a game's folder)")
    parser.add_argument("-data-out", dest="data_out", metavar="DIR", help="with -out: where material descriptions go (the runtime folder)")
    parser.add_argument("-defaults", action="store_true", help="also cook the shared MaterialDefault vertex shaders")
    parser.add_argument("-cache", metavar="FILE", help="write one shader cache instead of loose files")
    parser.add_argument("-module", default="", metavar="NAME", help="the module the cache belongs to")
    parser.add_argument("-gdi-scripts", default=str(GDI_SCRIPTS_DIR), metavar="DIR",
                        help="where GDI's cook_shaders.py lives (default: beside this module)")
    args = parser.parse_args()

    cs = _import_cook_shaders(args.gdi_scripts)

    if args.check:
        sys.exit(1 if check(cs, args.check, args.root) else 0)
    if args.target and (args.src or args.defaults) and (args.out or args.cache):
        if args.cache and not args.module:
            parser.error("-cache needs -module")
        sys.exit(1 if cook(cs, args.target, args.src or [], args.out, args.subdir, args.defaults, args.cache,
                           args.module, args.root, args.data_out) else 0)

    parser.error("give -check FILE..., or -target with -src (or -defaults) and -out or -cache")


if __name__ == "__main__":
    main()
