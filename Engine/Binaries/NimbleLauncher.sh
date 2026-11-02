#!/usr/bin/env sh
# NimbleLauncher: File > New Project without building the editor.
#
#     Engine/Binaries/NimbleLauncher.sh [--no-warmup]
#
# Configures Engine/Build/launcher the first time (which also prebuilds the
# third-party libraries into Engine/Intermediate/ThirdParty, shared with every
# game's tree), builds only NimbleLauncher and EngineCore, and starts it.
# Needs cmake and Ninja on PATH (or $CMAKE), and Qt 6.8+ where CMake
# finds it (Qt6_DIR or CMAKE_PREFIX_PATH).
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
BUILD="$HERE/../Build/launcher"
CMAKE=${CMAKE:-cmake}

if ! command -v "$CMAKE" >/dev/null 2>&1; then
    echo "NimbleLauncher: cmake not found; install it or set CMAKE to its path" >&2
    exit 1
fi

if [ ! -f "$BUILD/build.ninja" ]; then
    echo "NimbleLauncher: configuring $BUILD -- the first time prebuilds the third-party libraries too"
    "$CMAKE" -S "$HERE/.." -B "$BUILD" -G Ninja -DCMAKE_BUILD_TYPE=RelWithDebInfo -DNIMBLE_TARGET=Editor \
        -DNIMBLE_BUILD_SANDBOX=OFF -DNIMBLE_BUILD_TESTS=OFF "-DNIMBLE_BINARIES_DIR=$BUILD/Binaries"
fi
"$CMAKE" --build "$BUILD" --target NimbleLauncher

case "$(uname -s)" in
    Darwin) PLATFORM=Mac ;;
    *)      PLATFORM=Linux ;;
esac
EXE="$BUILD/Binaries/${PLATFORM}_RelWithDebInfo/NimbleLauncher"
if [ ! -x "$EXE" ]; then
    echo "NimbleLauncher: built, but $EXE is missing" >&2
    exit 1
fi
exec "$EXE" "$@"
