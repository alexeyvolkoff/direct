#!/usr/bin/env bash
# Build a single static omd-tunnel.exe via MXE (cross-compiled on Linux).
# Requires MXE with the x86_64-w64-mingw32.static target and the qtbase +
# openssl packages built (in a MXE checkout: make qtbase openssl).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJ_DIR="$ROOT/omd-tunnel"

MXE_TARGET="${MXE_TARGET:-x86_64-w64-mingw32.static}"
QMAKE="${MXE_TARGET}-qmake-qt5"
MXE_CMAKE="${MXE_TARGET}-cmake"
REF="${LDATACHANNEL_REF:-51085b8de4e6185dc019e3705c88b87933d7c3f6}"

# Ensure ccache uses a writable directory for non-root CI runner
export CCACHE_DIR="${CCACHE_DIR:-/tmp/mxe-ccache}"
mkdir -p "$CCACHE_DIR"

if ! command -v "$QMAKE" >/dev/null 2>&1; then
    echo "MXE qmake '$QMAKE' not found in PATH (is MXE built and sourced?)" >&2
    exit 1
fi

LDATACHANNEL_ROOT="${LDATACHANNEL_ROOT:-$ROOT/libdatachannel}"
LDATACHANNEL_BUILD="$LDATACHANNEL_ROOT/build-mxe"

CMAKE_CMD=()
if command -v "$MXE_CMAKE" >/dev/null 2>&1; then
    CMAKE_CMD=("$MXE_CMAKE")
elif command -v cmake >/dev/null 2>&1; then
    echo "==> '$MXE_CMAKE' not found, falling back to system cmake with MXE toolchain"
    QMAKE_PATH="$(command -v "$QMAKE")"
    MXE_BIN_DIR="$(dirname "$QMAKE_PATH")"
    TOOLCHAIN_FILE=""
    for cand in "$MXE_BIN_DIR/../$MXE_TARGET/share/cmake/mxe-conf.cmake" \
                "$MXE_BIN_DIR/../../$MXE_TARGET/share/cmake/mxe-conf.cmake" \
                "/opt/mxe/usr/$MXE_TARGET/share/cmake/mxe-conf.cmake"; do
        if [ -f "$cand" ]; then
            TOOLCHAIN_FILE="$cand"
            break
        fi
    done
    if [ -n "$TOOLCHAIN_FILE" ]; then
        CMAKE_CMD=("cmake" "-DCMAKE_TOOLCHAIN_FILE=$TOOLCHAIN_FILE")
    else
        CMAKE_CMD=("cmake" "-DCMAKE_SYSTEM_NAME=Windows" \
                   "-DCMAKE_C_COMPILER=${MXE_TARGET}-gcc" \
                   "-DCMAKE_CXX_COMPILER=${MXE_TARGET}-g++" \
                   "-DCMAKE_RC_COMPILER=${MXE_TARGET}-windres")
    fi
fi

if [ ${#CMAKE_CMD[@]} -gt 0 ]; then
    echo "==> Checking libdatachannel for $MXE_TARGET using ${CMAKE_CMD[*]}"
    if [ ! -d "$LDATACHANNEL_ROOT/.git" ]; then
        echo "==> Cloning libdatachannel into $LDATACHANNEL_ROOT"
        git clone --depth 1 --recurse-submodules --shallow-submodules \
            "${LDATACHANNEL_URL:-https://github.com/paullouisageneau/libdatachannel.git}" "$LDATACHANNEL_ROOT"
    fi

    CURRENT_REF="$(git -C "$LDATACHANNEL_ROOT" rev-parse HEAD 2>/dev/null || true)"
    if [ "$CURRENT_REF" != "$REF" ]; then
        if git -C "$LDATACHANNEL_ROOT" fetch --depth 1 origin "$REF" 2>/dev/null; then
            git -C "$LDATACHANNEL_ROOT" checkout -q "$REF"
            git -C "$LDATACHANNEL_ROOT" submodule update --init --recursive --depth 1
        fi
    fi

    if [ -f "$LDATACHANNEL_BUILD/libdatachannel.a" ] && [ -f "$LDATACHANNEL_BUILD/CMakeCache.txt" ]; then
        if grep -q "$LDATACHANNEL_ROOT" "$LDATACHANNEL_BUILD/CMakeCache.txt" 2>/dev/null; then
            echo "==> libdatachannel (mingw) is up to date at $LDATACHANNEL_BUILD"
        else
            echo "==> Clearing MXE CMakeCache.txt for path update"
            rm -f "$LDATACHANNEL_BUILD/CMakeCache.txt"
            "${CMAKE_CMD[@]}" -S "$LDATACHANNEL_ROOT" -B "$LDATACHANNEL_BUILD" \
                -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_MEDIA=ON -DNO_TESTS=ON \
                -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DBUILD_SHARED_LIBS=OFF
            cmake --build "$LDATACHANNEL_BUILD" --target datachannel -j"$(nproc)"
        fi
    else
        "${CMAKE_CMD[@]}" -S "$LDATACHANNEL_ROOT" -B "$LDATACHANNEL_BUILD" \
            -DCMAKE_BUILD_TYPE=Release -DNO_EXAMPLES=ON -DNO_MEDIA=ON -DNO_TESTS=ON \
            -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DBUILD_SHARED_LIBS=OFF
        cmake --build "$LDATACHANNEL_BUILD" --target datachannel -j"$(nproc)"
    fi
else
    echo "WARNING: neither '$MXE_CMAKE' nor system 'cmake' found; omd-tunnel will be relay-only" >&2
    LDATACHANNEL_ROOT=""
fi

BUILD_DIR="$ROOT/build-mxe"
rm -rf "$BUILD_DIR" "$ROOT/build" "$ROOT/bin"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

QMAKE_ARGS=("$PROJ_DIR/omd-tunnel.pro" CONFIG+=release CONFIG+=static)
if [ -n "$LDATACHANNEL_ROOT" ] && [ -f "$LDATACHANNEL_BUILD/libdatachannel.a" ]; then
    QMAKE_ARGS+=("LDATACHANNEL_ROOT=$LDATACHANNEL_ROOT" "LDATACHANNEL_BUILD=$LDATACHANNEL_BUILD")
fi
"$QMAKE" "${QMAKE_ARGS[@]}"
make -j"$(nproc)"

if [ "${OMD_TUNNEL_P2P_REQUIRED:-0}" = "1" ] || [ "${OMD_TUNNEL_P2P_REQUIRED:-}" = "true" ]; then
    if ! grep -q "P2P_ENABLED" Makefile*; then
        echo "ERROR: libdatachannel is not linked for Windows (P2P_ENABLED missing)." >&2
        echo "       Ensure MXE cmake ($MXE_CMAKE) is available." >&2
        exit 1
    fi
fi

EXE="$ROOT/bin/omd-tunnel.exe"
if [ -f "$EXE" ]; then
    echo "==> Artifact: $EXE"
else
    echo "Expected $EXE not found" >&2
    exit 1
fi

