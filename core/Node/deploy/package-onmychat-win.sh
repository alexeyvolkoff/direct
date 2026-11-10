#!/usr/bin/env bash
# Package the OnMyChat python app for the Windows per-user installer.
# Produces onmychat-latest.zip (code + requirements + config example, no venv/user data).
# Usage:
#   ./Node/deploy/package-onmychat-win.sh [onmychat_dir] [out_zip]
#   scp out_zip repo@forge.disk.direct:/opt/repo/Windows/onmychat-latest.zip
set -euo pipefail

SRC="${1:-${ONMYCHAT_DIR:-/home/alexey/projects/omd/onmychat}}"
OUT="${2:-$(pwd)/onmychat-latest.zip}"
# resolve OUT to an absolute path BEFORE cd into SRC (relative OUT must be
# relative to the caller's cwd, not to the onmychat checkout)
if [ ! "$(dirname "$OUT")" = / ]; then
    OUT="$(cd "$(dirname "$OUT")" && pwd)/$(basename "$OUT")"
fi

if [ ! -f "$SRC/api.py" ]; then
    echo "ERROR: $SRC/api.py not found - pass the onmychat checkout as the first argument" >&2
    exit 1
fi

rm -f "$OUT"
# exclude runtime/state dirs and venvs, keep code/prompts/docs; zip with relative paths
# so Expand-Archive unpacks api.py directly into $AiDir
( cd "$SRC" && zip -qr "$OUT" . \
    -x "venv/*" ".venv/*" "*__pycache__*" "user_data/*" "user-context/*" \
    -x "memory_index/*" "index/*" "history/*" "avatars/*" "data/*" \
    -x "asset/*" "*.pyc" "logs/*" "*.log" ".git/*" ".knowledge/*" ".ruff_cache/*" \
    -x "extensions/*" "chroma_db/*" "test_data/*" "*.tar.gz" \
    -x "config.ini" "agent_sessions.json" "index_exclusions.json" \
    -x "push_subscriptions.json" "vapid_keys.json" )

# Include initial knowledge base snapshot and default avatar
( cd "$SRC" && zip -ur "$OUT" memory_index/omd.jsonl avatars/June.png avatars/june.png >/dev/null 2>&1 || true )

echo "==> Artifact: $OUT"
# head closes the pipe early -> unzip dies with SIGPIPE(141) under pipefail;
# awk consumes the whole stream instead
unzip -l "$OUT" | awk 'NR<=16'
