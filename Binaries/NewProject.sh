#!/bin/sh
# A new game from Engine/Templates/Game, without the editor (NewProject.cmake).
#
#     Engine/Binaries/NewProject.sh <Name> [<folder>]
#
# <folder> defaults to Games/ beside Engine/. Needs cmake on PATH (or $CMAKE).
set -e

if [ $# -lt 1 ] || [ $# -gt 2 ]; then
    echo "Usage: $0 <Name> [<folder>]" >&2
    exit 2
fi

CMAKE="${CMAKE:-cmake}"
if ! command -v "$CMAKE" >/dev/null 2>&1; then
    echo "NewProject: cmake not found; install it or set CMAKE to its path" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$CMAKE" "-DNAME=$1" "-DLOCATION=${2:-}" -P "$SCRIPT_DIR/NewProject.cmake"
