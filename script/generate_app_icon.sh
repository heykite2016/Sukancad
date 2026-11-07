#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
    echo "usage: $0 /path/to/icon.svg /path/to/CadLens.icns" >&2
    exit 2
fi

SOURCE_SVG="$1"
OUTPUT_ICNS="$2"
WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT

SOURCE_PNG="$WORK_DIR/icon-1024.png"
ICONSET="$WORK_DIR/CadLens.iconset"
mkdir -p "$ICONSET"

sips -s format png "$SOURCE_SVG" --out "$SOURCE_PNG" >/dev/null

make_icon() {
    local size="$1"
    local name="$2"
    sips -z "$size" "$size" "$SOURCE_PNG" \
        --out "$ICONSET/$name" >/dev/null
}

make_icon 16 icon_16x16.png
make_icon 32 icon_16x16@2x.png
make_icon 32 icon_32x32.png
make_icon 64 icon_32x32@2x.png
make_icon 128 icon_128x128.png
make_icon 256 icon_128x128@2x.png
make_icon 256 icon_256x256.png
make_icon 512 icon_256x256@2x.png
make_icon 512 icon_512x512.png
cp "$SOURCE_PNG" "$ICONSET/icon_512x512@2x.png"

mkdir -p "$(dirname "$OUTPUT_ICNS")"
iconutil -c icns "$ICONSET" -o "$OUTPUT_ICNS"
