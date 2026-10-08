#!/bin/sh
# Install fanctld: root fan daemon + LaunchDaemon registration.
# Run with sudo:  sudo sh install-daemon.sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$ROOT_DIR"

if [ "$(id -u)" -ne 0 ]; then
    echo "Run with sudo: sudo sh install-daemon.sh" >&2
    exit 1
fi

BIN_DIR="$(swift build -c release --show-bin-path)"
# Do not build as root (root-owned .build, toolchain/PATH issues).
# Build first as user: sh build-app.sh
if [ ! -f "$BIN_DIR/fanctld" ]; then
    echo "fanctld not found in $BIN_DIR. Build first as user (without sudo): sh build-app.sh" >&2
    exit 1
fi
install -d -o root -g wheel -m 755 /usr/local/libexec/nvidiapowergadget
install -o root -g wheel -m 755 "$BIN_DIR/fanctld" /usr/local/libexec/nvidiapowergadget/fanctld
install -o root -g wheel -m 644 "$ROOT_DIR/com.nvidiapowergadget.fanctl.plist" \
    /Library/LaunchDaemons/com.nvidiapowergadget.fanctl.plist
# Shared config directory: root:admin, group-writable. The daemon (root)
# reads it, admin users (GUI) write it; non-admins get a read-only monitor.
install -d -o root -g admin -m 775 "/Library/Application Support/NVIDIAPowerGadget"

launchctl bootout system/com.nvidiapowergadget.fanctl 2>/dev/null || true
launchctl bootstrap system /Library/LaunchDaemons/com.nvidiapowergadget.fanctl.plist

echo "fanctld installed and started (log: /var/log/nvidiapowergadget-fanctl.log)"
