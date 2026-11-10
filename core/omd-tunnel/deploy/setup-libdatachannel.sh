#!/usr/bin/env bash
# Build libdatachannel (static) into the repo so that common/libdatachannel.pri
# can link it into omd-tunnel. Used by CI (and local builds) before qmake.
#
# Overrides:
#   LDATACHANNEL_DEST  target dir (default: <repo>/libdatachannel)
#   LDATACHANNEL_URL   git URL (default: official libdatachannel repo)
#   LDATACHANNEL_REF   git ref to build (default: pinned commit used for testing)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEST="${LDATACHANNEL_DEST:-$ROOT/libdatachannel}"
BUILD_DIR="$DEST/build-static"
URL="${LDATACHANNEL_URL:-https://github.com/paullouisageneau/libdatachannel.git}"
REF="${LDATACHANNEL_REF:-51085b8de4e6185dc019e3705c88b87933d7c3f6}"

require() { command -v "$1" >/dev/null 2>&1 || { echo "Missing required tool: $1" >&2; exit 1; }; }
require git
require cmake
require make

# 1. Clone or update repository (incremental)
if [ ! -d "$DEST/.git" ]; then
    echo "==> Cloning libdatachannel into $DEST"
    git clone --depth 1 --recurse-submodules --shallow-submodules "$URL" "$DEST"
fi

CURRENT_REF="$(git -C "$DEST" rev-parse HEAD 2>/dev/null || true)"
if [ "$CURRENT_REF" != "$REF" ]; then
    echo "==> Fetching pinned ref $REF"
    if git -C "$DEST" fetch --depth 1 origin "$REF" 2>/dev/null; then
        git -C "$DEST" checkout -q "$REF"
        git -C "$DEST" submodule update --init --recursive --depth 1
    fi
fi

# 2. Fast check: if target static lib exists and CMakeCache matches current path, incremental check
if [ -f "$BUILD_DIR/libdatachannel.a" ] && [ -f "$BUILD_DIR/CMakeCache.txt" ]; then
    if grep -q "$DEST" "$BUILD_DIR/CMakeCache.txt" 2>/dev/null; then
        echo "==> libdatachannel (linux static) is up to date at $BUILD_DIR"
        exit 0
    else
        echo "==> Path changed for CMakeCache.txt, clearing cache..."
        rm -f "$BUILD_DIR/CMakeCache.txt"
    fi
fi

echo "==> Configuring libdatachannel (static build)"
cmake -S "$DEST" -B "$BUILD_DIR" \
    -DCMAKE_C_COMPILER="$(command -v gcc || command -v cc)" \
    -DCMAKE_CXX_COMPILER="$(command -v g++ || command -v c++)" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_EXAMPLES=ON -DNO_MEDIA=ON -DNO_TESTS=ON \
    -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DPREFER_SYSTEM_LIB=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON

cmake --build "$BUILD_DIR" --target datachannel -j"$(nproc)"

echo "==> libdatachannel built at $BUILD_DIR"
