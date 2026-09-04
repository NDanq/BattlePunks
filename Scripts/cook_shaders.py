#!/usr/bin/env python3
"""
cook_shaders.py -- Offline shader preprocessing and compilation.

Usage:
    python cook_shaders.py -target <OpenGL|Vulkan|Metal>

Output layout:
    Shaders/Cooked/OpenGL/     -- preprocessed GLSL + OGLShaders_Cooked.qrc
    Shaders/Cooked/Vulkan/  -- SPIR-V .spv files + VulkanShaders_Cooked.qrc
    Shaders/Cooked/Metal/   -- stub MetalShaders_Cooked.qrc (not yet implemented)
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

SCRIPT_DIR  = Path(__file__).parent.resolve()
SHADERS_SRC = SCRIPT_DIR.parent / "Shaders"
COOKED_BASE = SHADERS_SRC / "Cooked"

VERTEX_SHADER = "VERTEX_SHADER"
PIXEL_SHADER  = "PIXEL_SHADER"
COMPUTE_SHADER = "COMPUTE_SHADER"

EXT_TO_TYPE = {
    ".vsh": VERTEX_SHADER,
    ".fsh": PIXEL_SHADER,
    ".csh": COMPUTE_SHADER
}

# glslc auto-detects stage from file extension
EXT_TO_GLSLC_EXT = {
    ".vsh": ".vert",
    ".fsh": ".frag",
    ".csh": ".comp"
}

PLATFORMS = ("OpenGL", "Vulkan", "Metal")


# ---------------------------------------------------------------------------
# Preprocessing  (mirrors LoadGLSLSource in GDIShader.cpp:200, extended)
# ---------------------------------------------------------------------------

# Compiled directive patterns
_RE_INCLUDE = re.compile(r'^#\s*include\s+<([^>]+)>')
_RE_DEFINE  = re.compile(r'^#\s*define\s+(\w+)')
_RE_UNDEF   = re.compile(r'^#\s*undef\s+(\w+)')
_RE_IFDEF   = re.compile(r'^#\s*ifdef\s+(\w+)')
_RE_IFNDEF  = re.compile(r'^#\s*ifndef\s+(\w+)')
_RE_IF      = re.compile(r'^#\s*if\b(.+)$')
_RE_ELSE    = re.compile(r'^#\s*else\b')
_RE_ENDIF   = re.compile(r'^#\s*endif\b')
_RE_VERSION = re.compile(r'^#\s*version\b')


def _substitute(line, defines):
    """Replace object-like defined symbol names with their expansion values."""
    for name, value in defines.items():
        if value:  # flag-only (empty string) defines have nothing to expand
            line = re.sub(rf'\b{re.escape(name)}\b', value, line)
    return line


def _expand_func_macros(line, func_macros):
    """Expand function-like macro calls: MACRO(arg1, arg2, ...) → body with args substituted."""
    for name, (params, body) in func_macros.items():
        pattern = rf'\b{re.escape(name)}\s*\(([^)]*)\)'
        def _replacer(m, params=params, body=body):
            args = [a.strip() for a in m.group(1).split(',')]
            result = body
            for param, arg in zip(params, args):
                result = re.sub(rf'\b{re.escape(param)}\b', arg, result)
            return result
        line = re.sub(pattern, _replacer, line)
    return line


def _eval_expr(expr, defines):
    """
    Evaluate a simple #if expression against the current defines dict.
    Supports: NAME, defined(NAME), !, &&, ||, ==, !=, <, >, <=, >=
    Unrecognised expressions produce a warning and evaluate to False.
    """
    expr = expr.strip()

    # defined(NAME) / defined NAME
    m = re.match(r'^defined\s*\(\s*(\w+)\s*\)$', expr)
    if m:
        return m.group(1) in defines
    m = re.match(r'^defined\s+(\w+)$', expr)
    if m:
        return m.group(1) in defines

    # Logical NOT
    m = re.match(r'^!\s*(.+)$', expr)
    if m:
        return not _eval_expr(m.group(1), defines)

    # Logical AND / OR (left-to-right, split on first occurrence)
    if '&&' in expr:
        lhs, rhs = expr.split('&&', 1)
        return _eval_expr(lhs, defines) and _eval_expr(rhs, defines)
    if '||' in expr:
        lhs, rhs = expr.split('||', 1)
        return _eval_expr(lhs, defines) or _eval_expr(rhs, defines)

    # Comparison operators (multi-char before single-char to avoid mis-splitting)
    for op in ('==', '!=', '<=', '>=', '<', '>'):
        if op in expr:
            lhs, rhs = expr.split(op, 1)
            lv = defines.get(lhs.strip(), lhs.strip())
            rv = defines.get(rhs.strip(), rhs.strip())
            try:
                ln, rn = float(lv), float(rv)
                return {'==': ln == rn, '!=': ln != rn,
                        '<':  ln <  rn, '>':  ln >  rn,
                        '<=': ln <= rn, '>=': ln >= rn}[op]
            except ValueError:
                print(f"WARNING: non-numeric comparison in #if: {expr}", file=sys.stderr)
                return False

    # Bare NAME: truthy if defined with a nonzero value, or defined as a flag
    name = expr.strip()
    if name in defines:
        val = defines[name]
        if not val:       # flag-only define → truthy
            return True
        try:
            return float(val) != 0
        except ValueError:
            return bool(val)

    return False  # undefined → falsy


def preprocess(file_path, shader_type, shaders_dir, defines=None,
               func_macros=None, _is_include=False):
    """
    Return the fully preprocessed GLSL source for the given shader type.

    defines:     dict[str, str] — object-like macros (name → value).
                 Seeded with {shader_type: "1"} plus any -cflag values.
                 Shared across recursive #include calls.
    func_macros: dict[str, (params, body)] — function-like macros.
                 Shared across recursive #include calls.
    _is_include: True when called recursively for an #include; causes
                 #version directives to be stripped (only the top-level
                 shader's version declaration is kept).
    """
    if defines is None:
        defines = {shader_type: "1"}
    if func_macros is None:
        func_macros = {}

    try:
        raw = file_path.read_text(encoding="utf-8")
    except OSError as exc:
        print(f"ERROR: Cannot read shader: {file_path}\n  {exc}", file=sys.stderr)
        sys.exit(1)

    # Stack entries: (should_emit: bool, seen_else: bool)
    cond_stack = []
    result     = []

    def _emitting():
        return not cond_stack or all(emit for emit, _ in cond_stack)

    for line in raw.split("\n"):
        s = line.strip()  # stripped copy used only for directive matching

        # ── #version (strip from included files) ────────────────────────────
        if _RE_VERSION.match(s):
            if not _is_include:
                result.append(line)
            continue

        # ── #include ────────────────────────────────────────────────────────
        m = _RE_INCLUDE.match(s)
        if m:
            if _emitting():
                include_path = shaders_dir / m.group(1).strip()
                result.append(preprocess(include_path, shader_type, shaders_dir,
                                         defines, func_macros, _is_include=True))
            continue

        # ── #define ─────────────────────────────────────────────────────────
        m = _RE_DEFINE.match(s)
        if m:
            if _emitting():
                name = m.group(1)
                rest = s[m.end():]          # everything after the macro name
                if rest.startswith('('):
                    # Function-like macro: parse params and body
                    paren_end    = rest.index(')')
                    params       = [p.strip() for p in rest[1:paren_end].split(',')
                                    if p.strip()]
                    body         = rest[paren_end + 1:].strip()
                    func_macros[name] = (params, body)
                else:
                    defines[name] = rest.strip()
            continue

        # ── #undef ──────────────────────────────────────────────────────────
        m = _RE_UNDEF.match(s)
        if m:
            if _emitting():
                name = m.group(1)
                defines.pop(name, None)
                func_macros.pop(name, None)
            continue

        # ── #ifdef / #ifndef ────────────────────────────────────────────────
        m = _RE_IFDEF.match(s)
        if m:
            name = m.group(1)
            cond_stack.append((name in defines or name in func_macros, False))
            continue

        m = _RE_IFNDEF.match(s)
        if m:
            name = m.group(1)
            cond_stack.append((name not in defines and name not in func_macros, False))
            continue

        # ── #if EXPR ────────────────────────────────────────────────────────
        m = _RE_IF.match(s)
        if m:
            cond_stack.append((_eval_expr(m.group(1), defines), False))
            continue

        # ── #else ───────────────────────────────────────────────────────────
        if _RE_ELSE.match(s):
            if cond_stack:
                emit, seen_else = cond_stack[-1]
                if not seen_else:
                    cond_stack[-1] = (not emit, True)
            continue

        # ── #endif ──────────────────────────────────────────────────────────
        if _RE_ENDIF.match(s):
            if cond_stack:
                cond_stack.pop()
            continue

        # ── Regular code line ────────────────────────────────────────────────
        if _emitting():
            out = _expand_func_macros(line, func_macros)
            out = _substitute(out, defines)
            result.append(out)

    return "\n".join(result)


# ---------------------------------------------------------------------------
# QRC generation
# ---------------------------------------------------------------------------

def write_qrc(out_dir, filenames, platform, stub_comment=None):
    qrc_path = out_dir / f"{platform}Shaders_Cooked.qrc"
    prefix   = f"/Shaders/Cooked/{platform}/"

    lines = ["<RCC>"]
    if stub_comment:
        lines.append(f"    <!-- {stub_comment} -->")
    lines.append(f'    <qresource prefix="{prefix}">')
    for name in sorted(filenames):
        lines.append(f"        <file>{name}</file>")
    lines.append("    </qresource>")
    lines.append("</RCC>")
    lines.append("")

    qrc_path.write_text("\n".join(lines), encoding="utf-8")
    print(f"  Written: {qrc_path.relative_to(SHADERS_SRC.parent)}")


# ---------------------------------------------------------------------------
# Platform handlers
# ---------------------------------------------------------------------------

def _collect_sources():
    sources = sorted(f for f in SHADERS_SRC.iterdir() if f.suffix in EXT_TO_TYPE)
    if not sources:
        print("ERROR: No .vsh or .fsh files found in Shaders/", file=sys.stderr)
        sys.exit(1)
    return sources


def cook_opengl(extra_defines=None):
    out_dir = COOKED_BASE / "OpenGL"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[OpenGL] Cooking shaders -> {out_dir.relative_to(SHADERS_SRC.parent)}")

    cooked = []
    for src in _collect_sources():
        shader_type = EXT_TO_TYPE[src.suffix]
        defines     = {shader_type: "1", "WITH_OPENGL": "1", **(extra_defines or {})}
        processed   = preprocess(src, shader_type, SHADERS_SRC, defines)
        (out_dir / src.name).write_text(processed, encoding="utf-8")
        cooked.append(src.name)
        print(f"  {src.name}")

    write_qrc(out_dir, cooked, "OpenGL")
    print(f"[OpenGL] Done -- {len(cooked)} shaders cooked.")


def _find_glslc():
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        candidate = Path(vulkan_sdk) / "Bin" / "glslc.exe"
        if candidate.is_file():
            return str(candidate)
    found = shutil.which("glslc")
    if found:
        return found
    print(
        "ERROR: glslc not found.\n"
        "  Set VULKAN_SDK to your Vulkan SDK root, or add glslc to PATH.",
        file=sys.stderr,
    )
    sys.exit(1)


def cook_vulkan(extra_defines=None):
    out_dir = COOKED_BASE / "Vulkan"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[Vulkan] Cooking shaders -> {out_dir.relative_to(SHADERS_SRC.parent)}")

    glslc = _find_glslc()
    print(f"  glslc: {glslc}")

    cooked = []
    for src in _collect_sources():
        shader_type = EXT_TO_TYPE[src.suffix]
        defines     = {shader_type: "1", "WITH_VULKAN": "1", **(extra_defines or {})}
        processed   = preprocess(src, shader_type, SHADERS_SRC, defines)
        spv_name    = src.name + ".spv"
        spv_path    = out_dir / spv_name

        tmp_fd, tmp_path = tempfile.mkstemp(suffix=EXT_TO_GLSLC_EXT[src.suffix])
        try:
            with os.fdopen(tmp_fd, "w", encoding="utf-8") as tmp:
                tmp.write(processed)
            result = subprocess.run(
                [glslc, tmp_path, "-o", str(spv_path)],
                capture_output=True,
                text=True,
            )
        finally:
            if os.path.exists(tmp_path):
                os.unlink(tmp_path)

        if result.returncode != 0:
            print(
                f"ERROR: glslc failed for {src.name}:\n{result.stderr}",
                file=sys.stderr,
            )
            sys.exit(1)

        cooked.append(spv_name)
        print(f"  {src.name} -> {spv_name}")

    write_qrc(out_dir, cooked, "Vulkan")
    print(f"[Vulkan] Done -- {len(cooked)} shaders compiled to SPIR-V.")


def _find_spirv_cross():
    vulkan_sdk = os.environ.get("VULKAN_SDK")
    if vulkan_sdk:
        for candidate_name in ("spirv-cross.exe", "spirv-cross"):
            candidate = Path(vulkan_sdk) / "Bin" / candidate_name
            if candidate.is_file():
                return str(candidate)
    found = shutil.which("spirv-cross")
    if found:
        return found
    print(
        "ERROR: spirv-cross not found.\n"
        "  Install the Vulkan SDK (which ships spirv-cross) or add it to PATH.\n"
        "  Alternatively run 'python cook_shaders.py -target Vulkan' first, then\n"
        "  install spirv-cross separately: https://github.com/KhronosGroup/SPIRV-Cross",
        file=sys.stderr,
    )
    sys.exit(1)


def cook_metal(extra_defines=None):
    """
    Transpile the already-compiled Vulkan SPIR-V binaries to Metal Shading Language
    using spirv-cross --msl.  Run 'cook_shaders.py -target Vulkan' first to ensure
    the .spv files are up to date.
    """
    spv_dir = COOKED_BASE / "Vulkan"
    out_dir = COOKED_BASE / "Metal"
    out_dir.mkdir(parents=True, exist_ok=True)
    print(f"[Metal] Transpiling SPIR-V -> MSL -> {out_dir.relative_to(SHADERS_SRC.parent)}")

    spv_files = sorted(spv_dir.glob("*.spv"))
    if not spv_files:
        print(
            "ERROR: No .spv files found in Cooked/Vulkan/.\n"
            "  Run 'python cook_shaders.py -target Vulkan' first.",
            file=sys.stderr,
        )
        sys.exit(1)

    spirv_cross = _find_spirv_cross()
    print(f"  spirv-cross: {spirv_cross}")

    cooked = []
    for spv in spv_files:
        # Strip the trailing .spv and append .metal, KEEPING the stage extension:
        # "Basic.vsh.spv" -> "Basic.vsh.metal". Dropping the stage (".vsh"/".fsh")
        # would collide the vertex and fragment outputs of a shader pair onto one
        # file, and GetCookedShaderPath (GDIShader.cpp) looks for the stage-suffixed
        # name anyway.
        metal_name = spv.stem + ".metal"    # e.g. "Basic.vsh.metal"
        metal_path = out_dir / metal_name

        result = subprocess.run(
            [
                spirv_cross, "--msl",
                # Keep the SPIR-V binding numbers as the MSL buffer()/texture()/
                # sampler() indices, so the Metal backend can bind by the same
                # indices the Vulkan descriptor conventions use.
                "--msl-decoration-binding",
                "--msl-version", "20200",
                str(spv), "--output", str(metal_path),
            ],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            print(
                f"ERROR: spirv-cross failed for {spv.name}:\n{result.stderr}",
                file=sys.stderr,
            )
            sys.exit(1)

        cooked.append(metal_name)
        print(f"  {spv.name} -> {metal_name}")

    write_qrc(out_dir, cooked, "Metal")
    print(f"[Metal] Done -- {len(cooked)} shaders transpiled to MSL.")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def main():
    parser = argparse.ArgumentParser(
        description="Cook GLSL shaders for a target render platform.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Examples:\n"
               "  python cook_shaders.py -target OpenGL\n"
               "  python cook_shaders.py -target Vulkan\n"
               "  python cook_shaders.py -target Metal",
    )
    parser.add_argument(
        "-target",
        required=True,
        choices=PLATFORMS,
        metavar="PLATFORM",
        help=f"Target platform: {', '.join(PLATFORMS)}",
    )
    parser.add_argument(
        "-cflag",
        action="append",
        metavar="NAME[=VALUE]",
        help="Define a preprocessor symbol (repeatable). "
             "NAME alone sets it to 1; NAME=VALUE sets it to VALUE. "
             "Factored into #if/#ifdef/#ifndef blocks during preprocessing.",
    )
    args = parser.parse_args()

    # Build the extra defines dict from all -cflag arguments
    extra_defines = {}
    for flag in (args.cflag or []):
        if "=" in flag:
            name, value = flag.split("=", 1)
            extra_defines[name.strip()] = value.strip()
        else:
            extra_defines[flag.strip()] = "1"

    {"OpenGL": cook_opengl, "Vulkan": cook_vulkan, "Metal": cook_metal}[args.target](extra_defines)


if __name__ == "__main__":
    main()
