#!/bin/sh
# A new game from Engine/Templates/Game, without the editor (NewProject.cmake).
#
#     Engine/Binaries/NewProject.sh <Name> [<folder>]
#     Engine/Binaries/NewProject.sh --example <Example> [--build [--jobs <N>]] [<Name> [<folder>]]
#
# <folder> defaults to Games/ beside Engine/. --example unpacks an example
# project from Engine/Examples instead (its own name unless <Name> is given);
# --build then configures, builds and cooks it, --jobs capping the build's jobs. Needs cmake on PATH (or $CMAKE).
set -e

EXAMPLE=""
BUILD=OFF
JOBS=""
while [ $# -gt 0 ]; do
    case "$1" in
        --example) [ $# -ge 2 ] || { echo "$0: --example needs a name" >&2; exit 2; }; EXAMPLE="$2"; shift 2 ;;
        --build)   BUILD=ON; shift ;;
        --jobs)    [ $# -ge 2 ] || { echo "$0: --jobs needs a number" >&2; exit 2; }; JOBS="$2"; shift 2 ;;
        *)         break ;;
    esac
done

if { [ -z "$EXAMPLE" ] && [ $# -lt 1 ]; } || [ $# -gt 2 ]; then
    echo "Usage: $0 <Name> [<folder>]" >&2
    echo "       $0 --example <Example> [--build [--jobs <N>]] [<Name> [<folder>]]" >&2
    exit 2
fi

CMAKE="${CMAKE:-cmake}"
if ! command -v "$CMAKE" >/dev/null 2>&1; then
    echo "NewProject: cmake not found; install it or set CMAKE to its path" >&2
    exit 1
fi

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
exec "$CMAKE" "-DNAME=${1:-}" "-DLOCATION=${2:-}" "-DEXAMPLE=$EXAMPLE" "-DBUILD=$BUILD" "-DJOBS=$JOBS" -P "$SCRIPT_DIR/NewProject.cmake"
