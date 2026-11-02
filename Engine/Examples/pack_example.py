#!/usr/bin/env python3
"""
pack_example.py -- an example project (a working Games/<Name>) into the
archives File > New Project unpacks, and its entry in Examples.json.

    python3 Engine/Examples/pack_example.py Games/DemoLab [--parts all|source|content] [--rev HEAD]

--rev packs the project as committed at that revision instead of as it is on
disk: git's own bytes (LF, whatever the checkout's line endings), git's
executable bits, and none of the uncommitted edits. Use it while the project
is tracked, and always on Windows, where a checkout has CRLF and no x bits.

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


def skipped_dir(name: str) -> bool:
    return name in EXCLUDED_DIRS or name.startswith("cmake-build-")


def wanted(rel: str) -> bool:
    """Whether a file (relative to the project, '/'-separated) goes in."""
    parts = rel.split("/")
    if len(parts) == 1:
        return bool(INCLUDED_FILES.match(rel))
    return (parts[0] in INCLUDED_DIRS and not any(skipped_dir(d) for d in parts[1:-1])
            and not EXCLUDED_FILES.match(parts[-1]))


class DiskTree:
    """The project as it is on disk."""

    def __init__(self, project: Path):
        self.project = project
        found = [entry.name for entry in project.iterdir() if entry.is_file()]
        for entry in project.iterdir():
            if entry.is_dir() and entry.name in INCLUDED_DIRS:
                for root, dirs, names in os.walk(entry):
                    dirs[:] = [d for d in dirs if not skipped_dir(d)]
                    found += [(Path(root) / name).relative_to(project).as_posix() for name in names]
        self.files = sorted(rel for rel in found if wanted(rel))

    def size(self, rel):
        return (self.project / rel).stat().st_size

    def executable(self, rel):
        return bool((self.project / rel).stat().st_mode & stat.S_IXUSR)

    def read(self, rels):
        return {rel: (self.project / rel).read_bytes() for rel in rels}


class GitTree:
    """The project as committed at a revision."""

    def __init__(self, project: Path, rev: str):
        self.project = project
        listing = subprocess.run(["git", "-C", str(project), "ls-tree", "-r", "-l", "-z", rev],
                                 check=True, capture_output=True).stdout
        self.blobs = {}
        for record in listing.split(b"\0"):
            if not record:
                continue
            meta, rel = record.split(b"\t", 1)
            mode, kind, sha, size = meta.split()
            if kind == b"blob" and mode in (b"100644", b"100755"):
                self.blobs[rel.decode("utf-8")] = (sha.decode(), int(size), mode == b"100755")
        self.files = sorted(rel for rel in self.blobs if wanted(rel))

    def size(self, rel):
        return self.blobs[rel][1]

    def executable(self, rel):
        return self.blobs[rel][2]

    def read(self, rels):
        shas = "".join(self.blobs[rel][0] + "\n" for rel in rels).encode()
        out = subprocess.run(["git", "-C", str(self.project), "cat-file", "--batch"],
                             input=shas, check=True, capture_output=True).stdout
        data, at = {}, 0
        for rel in rels:
            header_end = out.index(b"\n", at)
            length = int(out[at:header_end].split()[2])
            data[rel] = out[header_end + 1:header_end + 1 + length]
            at = header_end + 1 + length + 1
        return data


def split_content(tree, files, max_raw):
    """The assets in path order, cut into parts of at most max_raw bytes before
    compression (one file bigger than that is a part on its own)."""
    parts, current, size = [], [], 0
    for rel in files:
        length = tree.size(rel)
        if current and size + length > max_raw:
            parts.append(current)
            current, size = [], 0
        current.append(rel)
        size += length
    if current:
        parts.append(current)
    return parts


def make_tar(tree, files) -> bytes:
    buffer = io.BytesIO()
    contents = tree.read(files)
    with tarfile.open(fileobj=buffer, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel in files:
            info = tarfile.TarInfo(rel)
            info.size  = len(contents[rel])
            info.mtime = MTIME
            info.mode  = 0o755 if tree.executable(rel) else 0o644
            info.uid = info.gid = 0
            info.uname = info.gname = ""
            tar.addfile(info, io.BytesIO(contents[rel]))
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


def write_part(tree, files, out: Path):
    data = compress(make_tar(tree, files))
    out.write_bytes(data)
    print(f"  {out.name}: {len(files)} files, {len(data) / 1e6:.1f} MB")
    return {"File": out.name, "Bytes": len(data), "Sha256": hashlib.sha256(data).hexdigest()}


def engine_version():
    text = (ENGINE_DIR / "CMakeLists.txt").read_text(encoding="utf-8")
    match = re.search(r"project\(NimbleEngine\s+VERSION\s+([0-9.]+)", text)
    return match.group(1) if match else ""


def game_module(tree, name: str):
    """[Game] Module in the project's DefaultEngine.ini, else <Name>Game."""
    ini = "Config/DefaultEngine.ini"
    section = ""
    if ini in tree.files:
        for line in tree.read([ini])[ini].decode("utf-8").splitlines():
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
    parser.add_argument("--rev", help="pack the project as committed at this git revision (e.g. HEAD), not as on disk")
    args = parser.parse_args()

    project = args.project.resolve()
    name = args.name or project.name
    if args.rev:
        try:
            tree = GitTree(project, args.rev)
        except subprocess.CalledProcessError as error:
            sys.exit(f"pack_example: git can't list {project} at {args.rev}: {error.stderr.decode(errors='replace').strip()}")
    else:
        tree = DiskTree(project)
    if "CMakeLists.txt" not in tree.files:
        sys.exit(f"pack_example: {project} has no CMakeLists.txt")
    if not re.match(r"^[A-Za-z][A-Za-z0-9_]*$", name):
        sys.exit(f"pack_example: '{name}' is not a project name")

    files   = tree.files
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

    print(f"Packing {name} from {project}" + (f" at {args.rev}" if args.rev else ""))
    parts = []
    if args.parts in ("all", "source"):
        parts.append(write_part(tree, source, args.out / f"{name}.Source.tar.zst"))
    elif f"{name}.Source.tar.zst" in old_parts:
        parts.append(old_parts[f"{name}.Source.tar.zst"])
    else:
        sys.exit(f"pack_example: {name} has no Source part in {index_path} yet; pack --parts all")

    content_prefix = f"{name}.Content."
    if args.parts in ("all", "content"):
        for stale in args.out.glob(f"{content_prefix}*.tar.zst"):
            stale.unlink()
        for index_n, chunk in enumerate(split_content(tree, content, int(args.max_part * 1024 * 1024)), start=1):
            parts.append(write_part(tree, chunk, args.out / f"{content_prefix}{index_n}.tar.zst"))
    else:
        parts.extend(p for f, p in old_parts.items() if f.startswith(content_prefix))

    for part in parts:
        if part["Bytes"] > 50 * 1000 * 1000:
            print(f"pack_example: warning: {part['File']} is over 50 MB; lower --max-part", file=sys.stderr)

    entry["Module"]        = game_module(tree, name)
    entry["Parts"]         = parts
    entry["Files"]         = len(files)
    entry["UnpackedBytes"] = sum(tree.size(f) for f in files)
    entry["EngineVersion"] = engine_version()
    index["Schema"] = SCHEMA
    examples.sort(key=lambda e: e.get("Name", ""))
    index_path.write_text(json.dumps({"Schema": index["Schema"], "Examples": examples}, indent=2) + "\n", encoding="utf-8", newline="\n")
    print(f"Updated {index_path}")


if __name__ == "__main__":
    main()
