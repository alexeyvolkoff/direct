#!/usr/bin/env bash
# Build an omd-tunnel .deb package for Ubuntu/Debian (amd64).
# Requires: qtbase5-dev, libssl-dev, dpkg-dev.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
PROJ_DIR="$ROOT/omd-tunnel"

REV="$(git -C "$ROOT" rev-list HEAD --count 2>/dev/null || echo 0)"
VERSION="1.5.${REV}"

BUILD_DIR="$ROOT/build-deb"
BIN_OUT="$ROOT/bin/omd-tunnel"

echo "==> Preparing libdatachannel (P2P support)"
if [ -n "${LDATACHANNEL_ROOT:-}" ]; then
    echo "Using LDATACHANNEL_ROOT=$LDATACHANNEL_ROOT"
elif [ -d "$ROOT/libdatachannel" ]; then
    echo "Using $ROOT/libdatachannel"
else
    "$ROOT/omd-tunnel/deploy/setup-libdatachannel.sh"
fi

echo "==> Building omd-tunnel (release)"
rm -rf "$BUILD_DIR" "$ROOT/build" "$ROOT/bin"
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"
qmake "$PROJ_DIR/omd-tunnel.pro" CONFIG+=release
make -j"$(nproc)"

if [ "${OMD_TUNNEL_P2P_REQUIRED:-1}" = "1" ] && ! grep -q "P2P_ENABLED" Makefile; then
    echo "ERROR: libdatachannel is not linked (P2P_ENABLED missing). " >&2
    echo "       Build setup-libdatachannel.sh or set LDATACHANNEL_ROOT." >&2
    exit 1
fi

echo "==> Staging .deb"
STAGE="$BUILD_DIR/stage"
rm -rf "$STAGE"
mkdir -p "$STAGE/DEBIAN" "$STAGE/usr/bin"

install -m 0755 "$BIN_OUT" "$STAGE/usr/bin/omd-tunnel"

cat > "$STAGE/DEBIAN/control" <<EOF
Package: omd-tunnel
Version: $VERSION
Section: net
Priority: optional
Architecture: amd64
Depends: libqt5core5a | libqt5core5t64, libqt5network5 | libqt5network5t64, libssl3
Maintainer: OMD <dev@onmydisk.net>
Description: TCP tunnel over an OMD gateway (ssh ProxyCommand)
 Standalone tunnel client that connects through an OMD gateway to a device
 by its LinkID and forwards stdin/stdout as a raw TCP stream.
EOF

echo "==> Building .deb"
dpkg-deb --build --root-owner-group "$STAGE" "$BUILD_DIR/omd-tunnel_${VERSION}_amd64.deb"

echo "==> Artifact: $BUILD_DIR/omd-tunnel_${VERSION}_amd64.deb"
