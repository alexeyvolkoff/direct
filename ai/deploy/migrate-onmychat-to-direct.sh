#!/usr/bin/env bash
# One-time cutover: onmychat.service from /home/alexey/projects/omd/onmychat -> /home/alexey/projects/direct/ai
# Run as alexey (uses sudo for systemctl/unit install). State dirs are MOVED (same filesystem, instant).
set -euo pipefail
OLD=/home/alexey/projects/omd/onmychat
NEW=/home/alexey/projects/direct/ai
STAMP=$(date +%Y%m%d_%H%M%S)

sudo systemctl stop onmychat

# runtime state (all gitignored in direct/ai); pre-existing dirs in NEW are kept as *.bak.$STAMP
for item in chroma_db data asset personas memory_index index user_data \
            agent_sessions.json vapid_keys.json push_subscriptions.json \
            horde_dreamer_data.json index_exclusions.json config.ini; do
  [ -e "$OLD/$item" ] || continue
  if [ -e "$NEW/$item" ] && [ ! -L "$NEW/$item" ]; then mv "$NEW/$item" "$NEW/$item.bak.$STAMP"; fi
  [ -L "$NEW/$item" ] && rm "$NEW/$item"
  if [ "$item" = config.ini ]; then cp -a "$OLD/$item" "$NEW/$item"; else mv "$OLD/$item" "$NEW/$item"; fi
  echo "moved $item"
done

[ -e "$NEW/extensions" ] || ln -s "$OLD/extensions" "$NEW/extensions"

sudo install -m 644 "$NEW/deploy/onmychat.service" /etc/systemd/system/onmychat.service
sudo systemctl daemon-reload
sudo systemctl start onmychat
sleep 3
systemctl --no-pager status onmychat | head -12
