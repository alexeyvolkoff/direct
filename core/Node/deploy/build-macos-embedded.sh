#!/usr/bin/env bash
# Build self-contained On My Disk 3.0 P2P Node + omd-tunnel bundle for macOS.
# Requires Homebrew packages: qt@5, openssl@3, cmake
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
ARCH="$(uname -m)"
if [ "$ARCH" = "arm64" ]; then
    PKG_ARCH="arm64"
else
    PKG_ARCH="x86_64"
fi

QT_PREFIX="$(brew --prefix qt@5)"
OPENSSL_PREFIX="$(brew --prefix openssl@3)"
export OPENSSL_ROOT_DIR="$OPENSSL_PREFIX"
export PATH="$QT_PREFIX/bin:$PATH"

QMAKE="$QT_PREFIX/bin/qmake"
NCPU="$(sysctl -n hw.ncpu 2>/dev/null || echo 4)"
REF="${LDATACHANNEL_REF:-51085b8de4e6185dc019e3705c88b87933d7c3f6}"

LDATACHANNEL_ROOT="${LDATACHANNEL_ROOT:-$ROOT/libdatachannel}"
LDATACHANNEL_BUILD="$LDATACHANNEL_ROOT/build-macos-$PKG_ARCH"

echo "==> [1/4] Building static libdatachannel ($PKG_ARCH) against OpenSSL ($OPENSSL_PREFIX)"
if [ ! -d "$LDATACHANNEL_ROOT/.git" ]; then
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

cmake -S "$LDATACHANNEL_ROOT" -B "$LDATACHANNEL_BUILD" \
    -DCMAKE_BUILD_TYPE=Release \
    -DNO_EXAMPLES=ON -DNO_MEDIA=ON -DNO_TESTS=ON \
    -DUSE_GNUTLS=OFF -DUSE_NICE=OFF -DBUILD_SHARED_LIBS=OFF \
    -DOPENSSL_ROOT_DIR="$OPENSSL_PREFIX"
cmake --build "$LDATACHANNEL_BUILD" --target datachannel -j"$NCPU"

DEFAULT_BV="$(git -C "$ROOT" rev-list --count HEAD 2>/dev/null || echo 6722)"
BV="${BUILD_VERSION:-$DEFAULT_BV}"

echo "==> [2/4] Building onmydisk node (build $BV)"
BUILD_NODE="$ROOT/build-macos-node"
rm -rf "$BUILD_NODE" "$ROOT/Node/target/usr/bin/onmydisk" "$ROOT/bin/omd-tunnel"
mkdir -p "$BUILD_NODE"
pushd "$BUILD_NODE" >/dev/null
"$QMAKE" "$ROOT/Node.pro" \
    CONFIG+=release CONFIG+=sdk_no_version_check \
    DEFINES+=P2P_ENABLED DEFINES+=RTC_STATIC \
    BUILD_VERSION="$BV" \
    LDATACHANNEL_ROOT="$LDATACHANNEL_ROOT" \
    LDATACHANNEL_BUILD="$LDATACHANNEL_BUILD"
make -j"$NCPU"
popd >/dev/null

echo "==> [3/4] Building omd-tunnel (build $BV)"
BUILD_TUNNEL="$ROOT/build-macos-tunnel"
rm -rf "$BUILD_TUNNEL"
mkdir -p "$BUILD_TUNNEL" "$ROOT/bin"
pushd "$BUILD_TUNNEL" >/dev/null
"$QMAKE" "$ROOT/omd-tunnel/omd-tunnel.pro" \
    CONFIG+=release CONFIG+=sdk_no_version_check \
    DEFINES+=P2P_ENABLED DEFINES+=RTC_STATIC \
    BUILD_VERSION="$BV" \
    LDATACHANNEL_ROOT="$LDATACHANNEL_ROOT" \
    LDATACHANNEL_BUILD="$LDATACHANNEL_BUILD"
make -j"$NCPU"
popd >/dev/null

NODE_BIN="$ROOT/Node/target/usr/bin/onmydisk"
TUNNEL_BIN="$ROOT/bin/omd-tunnel"
test -x "$NODE_BIN"
test -x "$TUNNEL_BIN"

echo "==> [4/4] Creating self-contained relocatable bundle"
STAGE="$ROOT/dist/OnMyDisk"
rm -rf "$ROOT/dist"
mkdir -p "$STAGE/bin" "$STAGE/etc" "$STAGE/lib" "$STAGE/Frameworks" "$STAGE/plugins/sqldrivers"

cp -f "$NODE_BIN" "$STAGE/bin/onmydisk"
cp -f "$TUNNEL_BIN" "$STAGE/bin/omd-tunnel"
cp -f "$ROOT/run/etc/config/onmydisk.conf" "$STAGE/etc/onmydisk.conf"

# Copy SQLite driver plugin from Qt5
if [ -f "$QT_PREFIX/plugins/sqldrivers/libqsqlite.dylib" ]; then
    cp -f "$QT_PREFIX/plugins/sqldrivers/libqsqlite.dylib" "$STAGE/plugins/sqldrivers/"
fi

cat > "$STAGE/bin/qt.conf" <<'EOF'
[Paths]
Prefix = ..
Plugins = plugins
Frameworks = Frameworks
Libraries = lib
EOF

# Recursively bundle all non-system dylibs & Qt frameworks and rewrite install names
python3 - "$STAGE" "$QT_PREFIX" <<'PY'
import os, re, shutil, subprocess, sys

stage, qt_prefix = sys.argv[1], sys.argv[2]
bin_dir = os.path.join(stage, "bin")
lib_dir = os.path.join(stage, "lib")
fw_dir = os.path.join(stage, "Frameworks")
plug_dir = os.path.join(stage, "plugins", "sqldrivers")

def otool_deps(path):
    out = subprocess.check_output(["otool", "-L", path], text=True)
    deps = []
    for line in out.splitlines()[1:]:
        m = re.match(r"\s+(\S+)\s+\(", line)
        if m:
            deps.append(m.group(1))
    return deps

def is_system(dep):
    return dep.startswith("/usr/lib/") or dep.startswith("/System/Library/")

def resolve_dep(dep, orig_dir):
    if dep.startswith("@loader_path/") and orig_dir:
        cand = os.path.normpath(os.path.join(orig_dir, dep[len("@loader_path/"):]))
        if os.path.exists(cand):
            return os.path.realpath(cand)
    if os.path.exists(dep):
        return os.path.realpath(dep)
    if dep.startswith("@rpath/"):
        rel = dep[len("@rpath/"):]
        for base in [os.path.join(qt_prefix, "lib"), "/opt/homebrew/lib", "/usr/local/lib"]:
            cand = os.path.join(base, rel)
            if os.path.exists(cand):
                return os.path.realpath(cand)
    return None

queue = [
    (os.path.join(bin_dir, "onmydisk"), bin_dir),
    (os.path.join(bin_dir, "omd-tunnel"), bin_dir),
]
for f in os.listdir(plug_dir):
    if f.endswith(".dylib"):
        queue.append((os.path.join(plug_dir, f), os.path.join(qt_prefix, "plugins", "sqldrivers")))

processed = set()

while queue:
    target, orig_dir = queue.pop(0)
    if target in processed:
        continue
    processed.add(target)
    os.chmod(target, 0o755)

    # Ensure @executable_path/../Frameworks and @executable_path/../lib rpaths on executables
    if os.path.dirname(target) == bin_dir:
        for rp in ["@executable_path/../Frameworks", "@executable_path/../lib"]:
            subprocess.run(["install_name_tool", "-add_rpath", rp, target],
                           stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)

    for dep in otool_deps(target):
        if is_system(dep) or dep.startswith("@executable_path/"):
            continue
        # Skip self-id line on dylibs/frameworks
        if os.path.basename(dep) == os.path.basename(target) and os.path.dirname(target) != bin_dir:
            continue

        real = resolve_dep(dep, orig_dir)
        if not real or os.path.abspath(real).startswith(os.path.abspath(stage) + os.sep):
            continue

        fw_match = re.search(r"(/[^/]+\.framework)/Versions/([^/]+)/([^/]+)$", real)
        if fw_match:
            src_fw_root = real[:real.index(".framework") + len(".framework")]
            fw_name = os.path.basename(src_fw_root)
            ver = fw_match.group(2)
            bin_name = fw_match.group(3)
            dst_fw_root = os.path.join(fw_dir, fw_name)
            dst_fw_bin = os.path.join(dst_fw_root, "Versions", ver, bin_name)
            if not os.path.exists(dst_fw_bin):
                os.makedirs(os.path.dirname(dst_fw_bin), exist_ok=True)
                shutil.copy2(real, dst_fw_bin)
                os.chmod(dst_fw_bin, 0o755)
                res_src = os.path.join(src_fw_root, "Versions", ver, "Resources")
                if os.path.isdir(res_src):
                    shutil.copytree(res_src, os.path.join(dst_fw_root, "Versions", ver, "Resources"), dirs_exist_ok=True)
                new_id = f"@rpath/{fw_name}/Versions/{ver}/{bin_name}"
                subprocess.check_call(["install_name_tool", "-id", new_id, dst_fw_bin])
                queue.append((dst_fw_bin, os.path.dirname(real)))
            new_ref = f"@rpath/{fw_name}/Versions/{ver}/{bin_name}"
            if dep != new_ref:
                subprocess.check_call(["install_name_tool", "-change", dep, new_ref, target])
        else:
            dylib_name = os.path.basename(real)
            dst_dylib = os.path.join(lib_dir, dylib_name)
            if not os.path.exists(dst_dylib):
                shutil.copy2(real, dst_dylib)
                os.chmod(dst_dylib, 0o755)
                new_id = f"@rpath/{dylib_name}"
                subprocess.check_call(["install_name_tool", "-id", new_id, dst_dylib])
                queue.append((dst_dylib, os.path.dirname(real)))
            new_ref = f"@rpath/{dylib_name}"
            if dep != new_ref:
                subprocess.check_call(["install_name_tool", "-change", dep, new_ref, target])

# Ad-hoc sign all Mach-O files (innermost libraries first, executables last)
to_sign = []
for root, _, files in os.walk(stage):
    for fn in files:
        p = os.path.join(root, fn)
        if root == bin_dir and fn in ("onmydisk", "omd-tunnel"):
            continue
        if fn.endswith(".dylib") or re.search(r"\.framework/Versions/[^/]+/[^/]+$", p):
            if os.path.isfile(p) and not os.path.islink(p):
                to_sign.append(p)

for p in to_sign:
    subprocess.run(["codesign", "--force", "--sign", "-", p], check=True)
for exe in ["onmydisk", "omd-tunnel"]:
    subprocess.run(["codesign", "--force", "--sign", "-", os.path.join(bin_dir, exe)], check=True)
PY

echo "==> Verifying relocated Mach-O linkage:"
otool -L "$STAGE/bin/onmydisk"
otool -L "$STAGE/bin/omd-tunnel"
if otool -L "$STAGE/bin/onmydisk" "$STAGE/bin/omd-tunnel" "$STAGE"/lib/*.dylib 2>/dev/null | grep -E "/opt/homebrew|/usr/local/Cellar|/usr/local/opt"; then
    echo "ERROR: Unbundled Homebrew path detected!" >&2
    exit 1
fi

"$STAGE/bin/omd-tunnel" --help >/dev/null
echo "==> Smoke test passed: omd-tunnel executed cleanly!"

OUT_TAR="$ROOT/dist/onmydisk-node-macos-${PKG_ARCH}.tar.gz"
tar -czf "$OUT_TAR" -C "$STAGE" .
ls -lh "$OUT_TAR"
