#!/usr/bin/env python3
"""
pack_example.py -- an example project (a working Games/<Name>) into the
archives File > New Project unpacks, and its entry in Examples.json.

    python3 Engine/Examples/pack_example.py Games/DemoLab [--parts all|source|content]

Writes, beside this script:

    <Name>.Source.tar.zst        everything but the assets -- small, repacked often
    <Name>.Content.<N>.tar.zst   the assets (ASSET_PREFIXES: Content/ and the
                                 docs' images), split so each part stays
                                 under --max-part (GitHub warns above 50 MB)

and updates <Name>'s entry in Examples.json: the parts, their sizes and
SHA-256s, the unpacked size and the engine version. Title, Description and
anything else written there by hand are kept.

Only the project's sources go in -- never Build/, Binaries/, Saved/, IDE
folders or Python caches (EXCLUDED_* below). The archives are reproducible:
files sorted, every entry owned by 0:0 with a fixed time and mode 644 (755
if executable), so packing an unchanged project writes the same bytes and
git sees no change. Same zstd version, same bytes.

Needs Python 3.14 (compression.zstd), else the zstd command line on PATH.
Engine/Binaries/NewProject.cmake (-DEXAMPLE=) unpacks them; Engine/Examples/
README.md says when to repack.
"""

import argparse
import hashlib
import io
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tarfile
from pathlib import Path

EXAMPLES_DIR = Path(__file__).resolve().parent
ENGINE_DIR   = EXAMPLES_DIR.parent

# Top-level entries of the project that go in; everything else is left out.
INCLUDED_DIRS  = {"Source", "Content", "Config", "Scripts", "Tools", "Docs", "Plugins"}
INCLUDED_FILES = re.compile(r"^(CMakeLists\.txt|CMakePresets\.json|README\.md|\.gitignore|.*\.(cpp|h))$")
# Anywhere below them.
EXCLUDED_DIRS  = {"Build", "Binaries", "Saved", "Intermediate", "__pycache__", ".idea", ".vs", ".vscode", ".cache"}
EXCLUDED_FILES = re.compile(r"^(CMakeUserPresets\.json|\.DS_Store|Thumbs\.db|Desktop\.ini|.*\.(pyc|log))$")

# What goes in the Content parts: big, binary, seldom changed.
ASSET_PREFIXES = ("Content/", "Docs/Images/")

MTIME       = 1767225600   # 2026-01-01T00:00:00Z: every entry, so the bytes never depend on when it was packed
ZSTD_LEVEL  = 19
SCHEMA      = 1            # Examples.json's layout; NewProject.cmake and ProjectTemplate.cpp read it


def collect(project: Path):
    """Every file that goes in, relative to the project, sorted."""
    files = []
    for entry in sorted(project.iterdir()):
        if entry.is_file() and INCLUDED_FILES.match(entry.name):
            files.append(entry.name)
        elif entry.is_dir() and entry.name in INCLUDED_DIRS:
            for root, dirs, names in os.walk(entry):
                dirs[:] = sorted(d for d in dirs if d not in EXCLUDED_DIRS and not d.startswith("cmake-build-"))
                for name in sorted(names):
                    if not EXCLUDED_FILES.match(name):
                        files.append((Path(root) / name).relative_to(project).as_posix())
    return sorted(files)


def split_content(project: Path, files, max_raw):
    """The assets in path order, cut into parts of at most max_raw bytes before
    compression (one file bigger than that is a part on its own)."""
    parts, current, size = [], [], 0
    for rel in files:
        length = (project / rel).stat().st_size
        if current and size + length > max_raw:
            parts.append(current)
            current, size = [], 0
        current.append(rel)
        size += length
    if current:
        parts.append(current)
    return parts


def make_tar(project: Path, files) -> bytes:
    buffer = io.BytesIO()
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel in files:
            path = project / rel
            info = tarfile.TarInfo(rel)
            info.size  = path.stat().st_size
            info.mtime = MTIME
            info.mode  = 0o755 if path.stat().st_mode & stat.S_IXUSR else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            with open(path, "rb") as handle:
                tar.addfile(info, handle)
    return buffer.getvalue()


def compress(data: bytes) -> bytes:
    try:
        from compression import zstd
        return zstd.compress(data, level=ZSTD_LEVEL)
    except ImportError:
        tool = shutil.which("zstd")
        if not tool:
            sys.exit("pack_example: needs Python 3.14 (compression.zstd) or zstd on PATH")
        return subprocess.run([tool, f"-{ZSTD_LEVEL}", "-T1", "-q", "-c"], input=data, check=True, capture_output=True).stdout


def write_part(project: Path, files, out: Path):
    data = compress(make_tar(project, files))
    out.write_bytes(data)
    print(f"  {out.name}: {len(files)} files, {len(data) / 1e6:.1f} MB")
    return {"File": out.name, "Bytes": len(data), "Sha256": hashlib.sha256(data).hexdigest()}


def engine_version():
    text = (ENGINE_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(NimbleEngine\s+VERSION\s+([0-9.]+)", text)
    return match.group(1) if match else ""


def game_module(project: Path, name: str):
    """[Game] Module in the project's DefaultEngine.ini, else <Name>Game."""
    ini = project / "Config" / "DefaultEngine.ini"
    section = ""
    if ini.is_file():
        for line in ini.read_text(encoding="utf-8").splitlines():
            line = line.strip()
            if line.startswith("["):
                section = line
            elif section == "[Game]" and line.split("=", 1)[0].strip() == "Module":
                return line.split("=", 1)[1].strip()
    return name + "Game"


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0].strip())
    parser.add_argument("project", type=Path, help="the project folder, e.g. Games/DemoLab")
    parser.add_argument("--name", help="the example's name (default: the folder's)")
    parser.add_argument("--parts", choices=["all", "source", "content"], default="all",
                        help="which archives to write (default: all)")
    parser.add_argument("--out", type=Path, default=EXAMPLES_DIR, help="where the archives and Examples.json go")
    parser.add_argument("--max-part", type=float, default=128,
                        help="assets per part, MB before compression (default 128: about 40 MB packed)")
    args = parser.parse_args()

    project = args.project.resolve()
    name = args.name or project.name
    if not (project / "CMakeLists.txt").is_file():
        sys.exit(f"pack_example: {project} has no CMakeLists.txt")
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", name):
        sys.exit(f"pack_example: '{name}' is not a project name")

    files   = collect(project)
    content = [f for f in files if f.startswith(ASSET_PREFIXES)]
    source  = [f for f in files if not f.startswith(ASSET_PREFIXES)]
    args.out.mkdir(parents=True, exist_ok=True)

    index_path = args.out / "Examples.json"
    index = json.loads(index_path.read_text(encoding="utf-8")) if index_path.is_file() else {}
    examples = index.setdefault("Examples", [])
    entry = next((e for e in examples if e.get("Name") == name), None)
    if entry is None:
        entry = {"Name": name, "Title": name, "Description": ""}
        examples.append(entry)
    old_parts = {p["File"]: p for p in entry.get("Parts", [])}

    print(f"Packing {name} from {project}")
    parts = []
    if args.parts in ("all", "source"):
        parts.append(write_part(project, source, args.out / f"{name}.Source.tar.zst"))
    elif f"{name}.Source.tar.zst" in old_parts:
        parts.append(old_parts[f"{name}.Source.tar.zst"])
    else:
        sys.exit(f"pack_example: {name} has no Source part in {index_path} yet; pack --parts all")

    content_prefix = f"{name}.Content."
    if args.parts in ("all", "content"):
        for stale in args.out.glob(f"{content_prefix}*.tar.zst"):
            stale.unlink()
        for index_n, chunk in enumerate(split_content(project, content, int(args.max_part * 1024 * 1024)), start=1):
            parts.append(write_part(project, chunk, args.out / f"{content_prefix}{index_n}.tar.zst"))
    else:
        parts.extend(p for f, p in old_parts.items() if f.startswith(content_prefix))

    for part in parts:
        if part["Bytes"] > 50 * 1000 * 1000:
            print(f"pack_example: warning: {part['File']} is over 50 MB; lower --max-part", file=sys.stderr)

    entry["Module"]        = game_module(project, name)
    entry["Parts"]         = parts
    entry["Files"]         = len(files)
    entry["UnpackedBytes"] = sum((project / f).stat().st_size for f in files)
    entry["EngineVersion"] = engine_version()
    index["Schema"] = SCHEMA
    examples.sort(key=lambda e: e.get("Name", ""))
    index_path.write_text(json.dumps({"Schema": index["Schema"], "Examples": examples}, indent=2) + "\n", encoding="utf-8")
    print(f"Updated {index_path}")


if __name__ == "__main__":
    main()
