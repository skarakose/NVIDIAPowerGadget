#!/bin/sh
set -eu

ROOT_DIR="$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)"
cd "$ROOT_DIR"

swift build -c release
BIN_DIR="$(swift build -c release --show-bin-path)"
APP="$ROOT_DIR/NVIDIAPowerGadget.app"

rm -rf "$APP"
mkdir -p "$APP/Contents/MacOS" "$APP/Contents/Resources"
cp "$BIN_DIR/NVIDIAPowerGadget" "$APP/Contents/MacOS/NVIDIAPowerGadget"
cp "$ROOT_DIR/Info.plist" "$APP/Contents/Info.plist"
chmod 755 "$APP/Contents/MacOS/NVIDIAPowerGadget"

printf 'Built %s\n' "$APP"
