#!/bin/bash

# Load settings from /etc/onmydisk/onmydisk.conf.user
CONFIG_FILE="/etc/onmydisk/onmydisk.conf.user"

if [ -f "$CONFIG_FILE" ]; then
  # Extract sitesRoot value from the configuration file
  ROOT_DIR=$(grep -E '^sitesRoot=' "$CONFIG_FILE" | cut -d'=' -f2)
else
  echo "Configuration file $CONFIG_FILE not found."
  exit 1
fi

if [ -z "$ROOT_DIR" ]; then
  echo "sitesRoot is not set in $CONFIG_FILE."
  exit 1
fi

echo "Serving sites from $ROOT_DIR"

for SITE in "$ROOT_DIR"/*; do
  if [ -d "$SITE" ] && [ -f "$SITE/_config.yml" ]; then
    SITE_NAME=$(basename "$SITE")
    PORT=$(grep -E '^port:' "$SITE/_config.yml" | awk '{print $2}')
    echo "Starting Jekyll server for $SITE_NAME on port $PORT..."

    # Start Jekyll in the background for each site
    (cd "$SITE" && JEKYLL_ENV=production bundle exec jekyll serve --host 0.0.0.0  --port "$PORT") &
  fi
done

# Wait for all background processes
wait
