#!/usr/bin/env bash
# ==============================================================================
# On My Disk 3.0 Node + OnMyChat Per-User Installer for macOS (No sudo needed)
# ==============================================================================
# Usage:
#   # Typical install (Node + OnMyChat in P2P Agentic AI mode):
#   curl -fsSL https://forge.disk.direct/repo/macOS/install-onmydisk.sh | bash -s -- --agentic
#
#   # Apple Silicon Mac with local Metal GPU inference (Node + OnMyChat + local Ollama):
#   curl -fsSL https://forge.disk.direct/repo/macOS/install-onmydisk.sh | bash -s -- --gpu
#
#   # Storage-only P2P node:
#   curl -fsSL https://forge.disk.direct/repo/macOS/install-onmydisk.sh | bash
#
#   # Uninstall:
#   curl -fsSL https://forge.disk.direct/repo/macOS/install-onmydisk.sh | bash -s -- --uninstall
# ==============================================================================
set -euo pipefail

WITH_AI=0
AGENTIC=0
GPU_MODE=0
NO_OLLAMA=0
UNINSTALL=0
INFERENCE_NODE="auto"
REPO_BASE="https://forge.disk.direct/repo"

while [ $# -gt 0 ]; do
    case "$1" in
        --agentic|-Agentic)
            WITH_AI=1
            AGENTIC=1
            NO_OLLAMA=1
            shift
            ;;
        --gpu|-Gpu)
            WITH_AI=1
            GPU_MODE=1
            shift
            ;;
        --with-ai|-WithAi)
            WITH_AI=1
            shift
            ;;
        --no-ollama|-NoOllama)
            NO_OLLAMA=1
            shift
            ;;
        --inference-node|-InferenceNode)
            INFERENCE_NODE="${2:-auto}"
            shift 2
            ;;
        --uninstall|-Uninstall)
            UNINSTALL=1
            shift
            ;;
        *)
            echo "Unknown option: $1" >&2
            exit 1
            ;;
    esac
done

APP_SUPPORT="$HOME/Library/Application Support"
INSTALL_DIR="$APP_SUPPORT/OnMyDisk"
AI_DIR="$APP_SUPPORT/OnMyChat"
LAUNCH_AGENTS="$HOME/Library/LaunchAgents"
NODE_PLIST="$LAUNCH_AGENTS/net.onmydisk.node.plist"
AI_PLIST="$LAUNCH_AGENTS/com.onmychat.ai.plist"
UID_NUM="$(id -u)"

stop_services() {
    launchctl bootout "gui/$UID_NUM" "$NODE_PLIST" 2>/dev/null || launchctl unload "$NODE_PLIST" 2>/dev/null || true
    launchctl bootout "gui/$UID_NUM" "$AI_PLIST" 2>/dev/null || launchctl unload "$AI_PLIST" 2>/dev/null || true
    pkill -x onmydisk 2>/dev/null || true
}

if [ "$UNINSTALL" -eq 1 ]; then
    echo "==> Stopping and removing On My Disk & OnMyChat..."
    stop_services
    rm -f "$NODE_PLIST" "$AI_PLIST" "$HOME/.local/bin/omd-tunnel"
    rm -rf "$INSTALL_DIR" "$AI_DIR"
    echo "==> On My Disk node uninstalled from $HOME/Library/Application Support."
    exit 0
fi

ARCH="$(uname -m)"
if [ "$ARCH" = "arm64" ]; then
    PKG_ARCH="arm64"
else
    PKG_ARCH="x86_64"
fi

TMP_DIR="$(mktemp -d /tmp/omd-macos-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> [1/3] Installing On My Disk 3.0 P2P Node ($PKG_ARCH) into $INSTALL_DIR..."
stop_services
mkdir -p "$INSTALL_DIR" "$LAUNCH_AGENTS" "$HOME/.local/bin"

TAR_URL="$REPO_BASE/macOS/onmydisk-node-macos-${PKG_ARCH}.tar.gz"
if ! curl -fsSL "$TAR_URL" -o "$TMP_DIR/node.tar.gz" 2>/dev/null; then
    if [ "$PKG_ARCH" != "arm64" ]; then
        echo "Native x86_64 macOS node bundle is not available yet (Apple Silicon arm64 required)." >&2
        exit 1
    fi
    echo "Failed to download $TAR_URL" >&2
    exit 1
fi

# Preserve existing user config & SSL certs across upgrades
if [ -d "$INSTALL_DIR/etc" ]; then
    cp -a "$INSTALL_DIR/etc" "$TMP_DIR/etc_backup"
fi

tar -xzf "$TMP_DIR/node.tar.gz" -C "$INSTALL_DIR"

if [ -d "$TMP_DIR/etc_backup" ]; then
    [ -f "$TMP_DIR/etc_backup/onmydisk.conf.user" ] && cp -f "$TMP_DIR/etc_backup/onmydisk.conf.user" "$INSTALL_DIR/etc/"
    [ -f "$TMP_DIR/etc_backup/device.id" ] && cp -f "$TMP_DIR/etc_backup/device.id" "$INSTALL_DIR/etc/"
    [ -d "$TMP_DIR/etc_backup/ssl" ] && cp -a "$TMP_DIR/etc_backup/ssl" "$INSTALL_DIR/etc/"
fi

# Strip macOS quarantine attribute
xattr -dr com.apple.quarantine "$INSTALL_DIR" 2>/dev/null || true
ln -sf "$INSTALL_DIR/bin/omd-tunnel" "$HOME/.local/bin/omd-tunnel"

cat > "$NODE_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>net.onmydisk.node</string>
    <key>ProgramArguments</key>
    <array>
        <string>$INSTALL_DIR/bin/onmydisk</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$INSTALL_DIR/bin</string>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$INSTALL_DIR/node.log</string>
    <key>StandardErrorPath</key>
    <string>$INSTALL_DIR/node.err.log</string>
</dict>
</plist>
EOF

launchctl bootstrap "gui/$UID_NUM" "$NODE_PLIST" 2>/dev/null || launchctl load -w "$NODE_PLIST"

if [ "$WITH_AI" -eq 1 ]; then
    echo "==> [2/3] Installing OnMyChat AI service ($AI_DIR, port 4000)..."
    mkdir -p "$AI_DIR"

    # Locate Python 3.10-3.13 without triggering Xcode CLT GUI dialog, or fetch portable CPython 3.12
    PY_BIN=""
    for cand in /opt/homebrew/bin/python3.12 /opt/homebrew/bin/python3.11 /opt/homebrew/bin/python3 \
                /usr/local/bin/python3.12 /usr/local/bin/python3 "$INSTALL_DIR/python/bin/python3"; do
        if [ -x "$cand" ]; then
            PY_BIN="$cand"
            break
        fi
    done
    if [ -z "$PY_BIN" ] && xcode-select -p >/dev/null 2>&1 && [ -x /usr/bin/python3 ]; then
        PY_BIN="/usr/bin/python3"
    fi
    if [ -z "$PY_BIN" ]; then
        echo "==> Downloading portable Python 3.12 runtime for macOS..."
        mkdir -p "$INSTALL_DIR/python"
        PBS_ARCH="aarch64"
        [ "$PKG_ARCH" = "x86_64" ] && PBS_ARCH="x86_64"
        PBS_URL="https://github.com/astral-sh/python-build-standalone/releases/download/20250212/cpython-3.12.9+20250212-${PBS_ARCH}-apple-darwin-install_only.tar.gz"
        curl -fsSL "$PBS_URL" | tar -xz --strip-components=1 -C "$INSTALL_DIR/python"
        xattr -dr com.apple.quarantine "$INSTALL_DIR/python" 2>/dev/null || true
        PY_BIN="$INSTALL_DIR/python/bin/python3"
    fi

    ZIP_URL="$REPO_BASE/OnMyChat/onmychat-latest.zip"
    if ! curl -fsSL "$ZIP_URL" -o "$TMP_DIR/onmychat.zip" 2>/dev/null; then
        curl -fsSL "$REPO_BASE/Windows/onmychat-latest.zip" -o "$TMP_DIR/onmychat.zip"
    fi
    unzip -qo "$TMP_DIR/onmychat.zip" -d "$AI_DIR"
    for sub in user_data memory_index history avatars data; do
        mkdir -p "$AI_DIR/$sub"
    done

    CFG="$AI_DIR/config.ini"
    if [ ! -f "$CFG" ]; then
        cp "$AI_DIR/config.example.ini" "$CFG"
    fi

    "$PY_BIN" - "$CFG" "$AI_DIR" "$AGENTIC" "$GPU_MODE" "$INFERENCE_NODE" <<'PY'
import re, sys
cfg_path, ai_dir, agentic, gpu_mode, infer_node = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
with open(cfg_path, "r", encoding="utf-8") as f:
    cfg = f.read()
cfg = re.sub(r"(?m)^APP_ROOT_DIR\s*=.*$", f"APP_ROOT_DIR = {ai_dir}", cfg)
cfg = re.sub(r"(?m)^#?GATEWAY_URL\s*=.*$", "GATEWAY_URL = https://disk.direct", cfg)
if agentic:
    cfg = re.sub(r"(?m)^#?AI_PROVIDER\s*=.*$", "AI_PROVIDER = agentic", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_NODE\s*=.*$", f"INFERENCE_NODE = {infer_node}", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_BIND_OLLAMA\s*=.*$", "INFERENCE_BIND_OLLAMA = 21434", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_BIND_COMFY\s*=.*$", "INFERENCE_BIND_COMFY = 28188", cfg)
elif gpu_mode:
    cfg = re.sub(r"(?m)^#?AI_PROVIDER\s*=.*$", "AI_PROVIDER = local", cfg)
with open(cfg_path, "w", encoding="utf-8") as f:
    f.write(cfg)
PY

    if [ ! -x "$AI_DIR/venv/bin/python" ]; then
        "$PY_BIN" -m venv "$AI_DIR/venv"
    fi
    "$AI_DIR/venv/bin/pip" install --quiet --upgrade pip
    echo "==> Installing OnMyChat Python dependencies..."
    "$AI_DIR/venv/bin/pip" install --quiet -r "$AI_DIR/requirements.txt"

    UNIFIED_DIR="$AI_DIR/memory_index/unified_index"
    if [ ! -f "$UNIFIED_DIR/chroma.sqlite3" ]; then
        echo "==> Downloading pre-indexed OMD knowledge base..."
        mkdir -p "$UNIFIED_DIR"
        curl -fsSL "$REPO_BASE/OnMyChat/chroma_omd.tar.gz" | tar -xz -C "$UNIFIED_DIR" || true
    fi

    HF_HUB_DIR="$HOME/.cache/huggingface/hub"
    HF_MODEL_DIR="$HF_HUB_DIR/models--sentence-transformers--paraphrase-multilingual-MiniLM-L12-v2"
    if [ ! -d "$HF_MODEL_DIR" ]; then
        echo "==> Downloading pre-cached SentenceTransformers embedding model..."
        mkdir -p "$HF_HUB_DIR"
        curl -fsSL "$REPO_BASE/OnMyChat/sentence_model.tar.gz" | tar -xz -C "$HF_HUB_DIR" || true
    fi

    if [ "$NO_OLLAMA" -eq 0 ]; then
        if ! command -v ollama >/dev/null 2>&1 && [ ! -d "/Applications/Ollama.app" ]; then
            echo "==> Downloading Ollama for macOS (Apple Silicon Metal acceleration)..."
            curl -fsSL "https://ollama.com/download/Ollama-darwin.zip" -o "$TMP_DIR/Ollama.zip"
            unzip -qo "$TMP_DIR/Ollama.zip" -d "/Applications"
            xattr -dr com.apple.quarantine "/Applications/Ollama.app" 2>/dev/null || true
            open -g "/Applications/Ollama.app" || true
        fi
    fi

    cat > "$AI_PLIST" <<EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
    <key>Label</key>
    <string>com.onmychat.ai</string>
    <key>ProgramArguments</key>
    <array>
        <string>$AI_DIR/venv/bin/python</string>
        <string>-m</string>
        <string>uvicorn</string>
        <string>api:app</string>
        <string>--host</string>
        <string>127.0.0.1</string>
        <string>--port</string>
        <string>4000</string>
    </array>
    <key>WorkingDirectory</key>
    <string>$AI_DIR</string>
    <key>EnvironmentVariables</key>
    <dict>
        <key>PATH</key>
        <string>$AI_DIR/venv/bin:/opt/homebrew/bin:/usr/local/bin:/usr/bin:/bin</string>
        <key>TOKENIZERS_PARALLELISM</key>
        <string>false</string>
    </dict>
    <key>RunAtLoad</key>
    <true/>
    <key>KeepAlive</key>
    <true/>
    <key>StandardOutPath</key>
    <string>$AI_DIR/onmychat.log</string>
    <key>StandardErrorPath</key>
    <string>$AI_DIR/onmychat.log</string>
</dict>
</plist>
EOF

    launchctl bootstrap "gui/$UID_NUM" "$AI_PLIST" 2>/dev/null || launchctl load -w "$AI_PLIST"
fi

echo ""
echo "==> [3/3] Installation complete!"
echo "    1. Sign in at https://disk.direct"
echo "    2. Open http://localhost:8081 to pair this Mac (or connect via 'Detected Devices' in Settings)"
if [ "$WITH_AI" -eq 1 ]; then
    if [ "$AGENTIC" -eq 1 ]; then
        echo "    3. OnMyChat is running on 127.0.0.1:4000 in Agentic P2P mode (local RAG + remote P2P GPU)."
    else
        echo "    3. OnMyChat is running on 127.0.0.1:4000 in Local Metal GPU mode (pull a model via: ollama pull gemma4)."
    fi
fi
