#!/usr/bin/env bash
# ==============================================================================
# On My Disk 3.0 Node + OnMyChat Installer for Linux (Ubuntu / Debian / Armbian)
# ==============================================================================
# Usage:
#   # Typical install (Node + OnMyChat in P2P Agentic AI mode — no local GPU needed):
#   curl -fsSL https://forge.disk.direct/repo/Linux/install-onmydisk.sh | bash -s -- --agentic
#
#   # Workstation / server with NVIDIA GPU (Node + OnMyChat + local Ollama):
#   curl -fsSL https://forge.disk.direct/repo/Linux/install-onmydisk.sh | bash -s -- --gpu
#
#   # Storage-only P2P node:
#   curl -fsSL https://forge.disk.direct/repo/Linux/install-onmydisk.sh | bash
#
#   # Uninstall:
#   curl -fsSL https://forge.disk.direct/repo/Linux/install-onmydisk.sh | bash -s -- --uninstall
# ==============================================================================
set -euo pipefail

WITH_AI=0
AGENTIC=0
GPU_MODE=0
AI_CPU=0
NO_OLLAMA=0
RKLLAMA=0
UNINSTALL=0
INFERENCE_NODE="auto"
REPO_BASE="https://forge.disk.direct/repo"

while [ $# -gt 0 ]; do
    case "$1" in
        --agentic|-Agentic)
            WITH_AI=1
            AGENTIC=1
            AI_CPU=1
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
        --ai-cpu|-AiCpu)
            AI_CPU=1
            shift
            ;;
        --no-ollama|-NoOllama)
            NO_OLLAMA=1
            shift
            ;;
        --rkllama)
            WITH_AI=1
            RKLLAMA=1
            AI_CPU=1
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

if [ "${EUID:-$(id -u)}" -eq 0 ]; then
    SUDO=""
    RUN_USER="${SUDO_USER:-root}"
else
    SUDO="sudo"
    RUN_USER="$USER"
fi

if [ "$RUN_USER" = "root" ]; then
    for cand in ubuntu debian firefly orange pi; do
        if id "$cand" >/dev/null 2>&1; then
            RUN_USER="$cand"
            break
        fi
    done
fi

run_as_user() {
    if [ "$(id -un)" = "$RUN_USER" ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        run_as_user "$@"
    else
        runuser -u "$RUN_USER" -- "$@"
    fi
}

RUN_GROUP="$(id -gn "$RUN_USER" 2>/dev/null || echo "$RUN_USER")"
RUN_HOME="$(getent passwd "$RUN_USER" | cut -d: -f6)"
[ -z "$RUN_HOME" ] && RUN_HOME="$(eval echo "~$RUN_USER")"

if [ "$UNINSTALL" -eq 1 ]; then
    echo "==> Stopping and removing On My Disk & OnMyChat services..."
    $SUDO systemctl stop onmychat 2>/dev/null || true
    $SUDO systemctl disable onmychat 2>/dev/null || true
    $SUDO rm -f /etc/systemd/system/onmychat.service
    $SUDO systemctl stop onmydisk 2>/dev/null || true
    $SUDO apt-get remove -y onmydisk-connector omd-tunnel 2>/dev/null || true
    $SUDO rm -rf /opt/onmychat
    $SUDO systemctl daemon-reload || true
    echo "==> On My Disk node uninstalled."
    exit 0
fi

ARCH="$(uname -m)"
case "$ARCH" in
    x86_64|amd64)
        DEB_URL="$REPO_BASE/OnMyDisk/Connector/onmydisk-connector-amd64.deb"
        TUNNEL_DEB_URL="$REPO_BASE/Ubuntu/focal/omd-tunnel-amd64.deb"
        ;;
    aarch64|arm64)
        DEB_URL="$REPO_BASE/OnMyDisk/Connector/onmydisk-connector-arm64.deb"
        TUNNEL_DEB_URL=""
        AI_CPU=1
        ;;
    armv7l|armhf)
        DEB_URL="$REPO_BASE/Ubuntu/focal/onmydisk-connector-armhf.deb"
        TUNNEL_DEB_URL=""
        AI_CPU=1
        ;;
    *)
        echo "Unsupported Linux architecture: $ARCH" >&2
        exit 1
        ;;
esac

TMP_DIR="$(mktemp -d /tmp/omd-install.XXXXXX)"
trap 'rm -rf "$TMP_DIR"' EXIT

echo "==> [1/3] Installing On My Disk 3.0 P2P Node ($ARCH)..."
$SUDO apt-get update -qq
$SUDO apt-get install -y -qq curl wget ca-certificates tar unzip
$SUDO apt-get install -y -qq rclone 2>/dev/null || true

curl -fsSL "$DEB_URL" -o "$TMP_DIR/onmydisk-connector.deb"
DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$TMP_DIR/onmydisk-connector.deb"

if [ -n "$TUNNEL_DEB_URL" ]; then
    if curl -fsSL "$TUNNEL_DEB_URL" -o "$TMP_DIR/omd-tunnel.deb" 2>/dev/null; then
        DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y "$TMP_DIR/omd-tunnel.deb" || true
    fi
fi

# Configure [FileNode] user/group and ensure 3.0 disk.direct:9010 defaults
CONF="/etc/onmydisk/onmydisk.conf"
if [ -f "$CONF" ]; then
    $SUDO python3 - "$CONF" "$RUN_USER" "$RUN_GROUP" <<'PY' || true
import configparser, sys
conf_path, user, group = sys.argv[1], sys.argv[2], sys.argv[3]
with open(conf_path, "r", encoding="utf-8", errors="replace") as f:
    text = f.read()
lines = text.splitlines()
out, in_fn, set_u, set_g = [], False, False, False
for line in lines:
    s = line.strip()
    if s.startswith("[") and s.endswith("]"):
        if in_fn:
            if not set_u: out.append(f"User={user}")
            if not set_g: out.append(f"Group={group}")
        in_fn = (s == "[FileNode]")
    elif in_fn and s.startswith("User="):
        line = f"User={user}"; set_u = True
    elif in_fn and s.startswith("Group="):
        line = f"Group={group}"; set_g = True
    out.append(line)
if in_fn:
    if not set_u: out.append(f"User={user}")
    if not set_g: out.append(f"Group={group}")
res = "\n".join(out) + "\n"
res = res.replace("host=onmydisk.net", "host=disk.direct")
res = res.replace("port=9000", "port=9010")
res = res.replace("wsPort=9988", "wsPort=9989")
with open(conf_path, "w", encoding="utf-8") as f:
    f.write(res)
PY
fi

$SUDO systemctl enable onmydisk >/dev/null 2>&1 || true
$SUDO systemctl restart onmydisk

if [ "$WITH_AI" -eq 1 ]; then
    echo "==> [2/3] Installing OnMyChat AI service (/opt/onmychat, port 4000)..."
    DEBIAN_FRONTEND=noninteractive $SUDO apt-get install -y -qq python3 python3-venv python3-pip poppler-utils pandoc ffmpeg

    $SUDO systemctl stop onmychat 2>/dev/null || true
    $SUDO mkdir -p /opt/onmychat
    $SUDO chown -R "$RUN_USER:$RUN_GROUP" /opt/onmychat

    ZIP_URL="$REPO_BASE/OnMyChat/onmychat-latest.zip"
    if ! curl -fsSL "$ZIP_URL" -o "$TMP_DIR/onmychat.zip" 2>/dev/null; then
        curl -fsSL "$REPO_BASE/Windows/onmychat-latest.zip" -o "$TMP_DIR/onmychat.zip"
    fi
    run_as_user unzip -qo "$TMP_DIR/onmychat.zip" -d /opt/onmychat
    for sub in user_data memory_index history avatars data; do
        run_as_user mkdir -p "/opt/onmychat/$sub"
    done

    CFG="/opt/onmychat/config.ini"
    if [ ! -f "$CFG" ]; then
        run_as_user cp /opt/onmychat/config.example.ini "$CFG"
    fi

    run_as_user python3 - "$CFG" "$AGENTIC" "$GPU_MODE" "$RKLLAMA" "$INFERENCE_NODE" <<'PY'
import re, sys
cfg_path, agentic, gpu_mode, rkllama, infer_node = sys.argv[1], int(sys.argv[2]), int(sys.argv[3]), int(sys.argv[4]), sys.argv[5]
with open(cfg_path, "r", encoding="utf-8") as f:
    cfg = f.read()
cfg = re.sub(r"(?m)^APP_ROOT_DIR\s*=.*$", "APP_ROOT_DIR = /opt/onmychat", cfg)
cfg = re.sub(r"(?m)^#?GATEWAY_URL\s*=.*$", "GATEWAY_URL = https://disk.direct", cfg)
if agentic:
    cfg = re.sub(r"(?m)^#?AI_PROVIDER\s*=.*$", "AI_PROVIDER = agentic", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_NODE\s*=.*$", f"INFERENCE_NODE = {infer_node}", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_BIND_OLLAMA\s*=.*$", "INFERENCE_BIND_OLLAMA = 21434", cfg)
    cfg = re.sub(r"(?m)^#?INFERENCE_BIND_COMFY\s*=.*$", "INFERENCE_BIND_COMFY = 28188", cfg)
elif gpu_mode or rkllama:
    cfg = re.sub(r"(?m)^#?AI_PROVIDER\s*=.*$", "AI_PROVIDER = local", cfg)
    if rkllama:
        cfg = re.sub(r"(?m)^#?OLLAMA_URL\s*=.*$", "OLLAMA_URL = http://localhost:8080", cfg)
with open(cfg_path, "w", encoding="utf-8") as f:
    f.write(cfg)
PY

    if [ ! -x /opt/onmychat/venv/bin/python ]; then
        run_as_user python3 -m venv /opt/onmychat/venv
    fi
    run_as_user /opt/onmychat/venv/bin/pip install --quiet --upgrade pip
    if [ "$AI_CPU" -eq 1 ] && [ "$GPU_MODE" -eq 0 ]; then
        echo "==> Installing lightweight CPU PyTorch wheels..."
        run_as_user /opt/onmychat/venv/bin/pip install --quiet torch --index-url https://download.pytorch.org/whl/cpu
    fi
    echo "==> Installing OnMyChat Python dependencies..."
    run_as_user /opt/onmychat/venv/bin/pip install --quiet -r /opt/onmychat/requirements.txt

    # Pre-cached OMD ChromaDB & multilingual embedding model
    UNIFIED_DIR="/opt/onmychat/memory_index/unified_index"
    if [ ! -f "$UNIFIED_DIR/chroma.sqlite3" ]; then
        echo "==> Downloading pre-indexed OMD knowledge base..."
        run_as_user mkdir -p "$UNIFIED_DIR"
        curl -fsSL "$REPO_BASE/OnMyChat/chroma_omd.tar.gz" | run_as_user tar -xz -C "$UNIFIED_DIR" || true
    fi

    HF_HUB_DIR="$RUN_HOME/.cache/huggingface/hub"
    HF_MODEL_DIR="$HF_HUB_DIR/models--sentence-transformers--paraphrase-multilingual-MiniLM-L12-v2"
    if [ ! -d "$HF_MODEL_DIR" ]; then
        echo "==> Downloading pre-cached SentenceTransformers embedding model..."
        run_as_user mkdir -p "$HF_HUB_DIR"
        curl -fsSL "$REPO_BASE/OnMyChat/sentence_model.tar.gz" | run_as_user tar -xz -C "$HF_HUB_DIR" || true
    fi

    if [ "$NO_OLLAMA" -eq 0 ]; then
        if ! command -v ollama >/dev/null 2>&1; then
            echo "==> Installing local Ollama engine..."
            curl -fsSL https://ollama.com/install.sh | sh
        fi
    fi

    $SUDO tee /etc/systemd/system/onmychat.service >/dev/null <<EOF
[Unit]
Description=OnMyChat AI Service (port 4000)
After=network-online.target onmydisk.service
Wants=network-online.target

[Service]
Type=simple
User=$RUN_USER
Group=$RUN_GROUP
WorkingDirectory=/opt/onmychat
Environment="PATH=/opt/onmychat/venv/bin:/usr/local/bin:/usr/bin:/bin"
Environment="TOKENIZERS_PARALLELISM=false"
ExecStart=/opt/onmychat/venv/bin/uvicorn api:app --host 127.0.0.1 --port 4000
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

    $SUDO systemctl daemon-reload
    $SUDO systemctl enable --now onmychat
    $SUDO systemctl restart onmychat
fi

LAN_IP="$(hostname -I 2>/dev/null | awk '{print $1}')"
echo ""
echo "==> [3/3] Installation complete!"
echo "    1. Sign in at https://disk.direct"
echo "    2. Open http://${LAN_IP:-localhost}/ (or connect from 'Detected Devices' in Settings)"
if [ "$WITH_AI" -eq 1 ]; then
    if [ "$AGENTIC" -eq 1 ]; then
        echo "    3. OnMyChat is running on 127.0.0.1:4000 in Agentic P2P mode (local RAG + remote P2P GPU)."
    else
        echo "    3. OnMyChat is running on 127.0.0.1:4000 in Local GPU mode (pull a model via: ollama pull gemma4)."
    fi
fi
