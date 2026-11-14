#!/bin/bash

# Generic post-installation script
echo "Running post-install script..."

# Ensure libcrypt.so.2 compatibility symlink on Ubuntu/Debian if only libcrypt.so.1 exists
for dir in /usr/lib/x86_64-linux-gnu /lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu /lib/aarch64-linux-gnu /usr/lib64 /lib64 /usr/lib /lib; do
    if [ -f "$dir/libcrypt.so.1" ] && [ ! -e "$dir/libcrypt.so.2" ]; then
        ln -sf "$dir/libcrypt.so.1" "$dir/libcrypt.so.2" 2>/dev/null || true
    fi
done
ldconfig 2>/dev/null || true

echo "Post-install script completed successfully."
exit 0
