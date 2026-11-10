#!/bin/bash
# ==============================================================================
# On My Disk OTA Package Updater Script
# ==============================================================================

LOG_FILE="/var/log/onmydisk-updater.log"
exec >> "$LOG_FILE" 2>&1

echo "=================================================="
echo "[$(date '+%Y-%m-%d %H:%M:%S')] Starting On My Disk OTA update..."

DEB_FILE="$1"
if [ -z "$DEB_FILE" ] || [ ! -f "$DEB_FILE" ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] ERROR: Deb package file '$DEB_FILE' not found!"
    exit 1
fi

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Target package: $DEB_FILE"

# Allow daemon process 2 seconds to release files and prepare for restart
sleep 2

export DEBIAN_FRONTEND=noninteractive

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Installing package via dpkg..."
dpkg -i "$DEB_FILE"
DPKG_STATUS=$?

if [ $DPKG_STATUS -ne 0 ]; then
    echo "[$(date '+%Y-%m-%d %H:%M:%S')] dpkg returned code $DPKG_STATUS. Attempting dependency fix with apt-get..."
    apt-get update -y
    apt-get install -f -y
fi

echo "[$(date '+%Y-%m-%d %H:%M:%S')] Restarting onmydisk service via systemd..."
systemctl daemon-reload
systemctl restart onmydisk

# Cleanup downloaded package
rm -f "$DEB_FILE"

echo "[$(date '+%Y-%m-%d %H:%M:%S')] On My Disk OTA update completed successfully."
echo "=================================================="
exit 0
