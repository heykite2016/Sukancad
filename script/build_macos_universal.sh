#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
QT_ROOT="${QT_ROOT:-$HOME/Library/Caches/CadLens/Qt/6.8.3/macos}"
BUILD_DIR="${CADLENS_MAC_BUILD_DIR:-$ROOT_DIR/build-macos-universal}"
APP="$BUILD_DIR/CadLens.app"
export PATH="$HOME/Library/Python/3.14/bin:$PATH"

cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
    -DCMAKE_PREFIX_PATH="$QT_ROOT" \
    -DBUILD_TESTING=ON
rm -f "$APP/Contents/MacOS/CadLens"
cmake --build "$BUILD_DIR" -j8

"$QT_ROOT/bin/macdeployqt" "$APP" -always-overwrite >/dev/null
"$ROOT_DIR/script/generate_app_icon.sh" \
    "$ROOT_DIR/resources/icons/logo.svg" \
    "$BUILD_DIR/CadLens.icns"
cp "$BUILD_DIR/CadLens.icns" "$APP/Contents/Resources/CadLens.icns"
/usr/libexec/PlistBuddy -c "Set :CFBundleIconFile CadLens.icns" \
    "$APP/Contents/Info.plist" 2>/dev/null \
    || /usr/libexec/PlistBuddy -c \
        "Add :CFBundleIconFile string CadLens.icns" \
        "$APP/Contents/Info.plist"
/usr/bin/strip -x "$APP/Contents/MacOS/CadLens"
/usr/bin/codesign --force --deep --options runtime \
    --entitlements "$ROOT_DIR/script/CadLens.entitlements" \
    --sign - "$APP"
/usr/bin/codesign --verify --deep --strict "$APP"

BACKUP_DIR="$(mktemp -d)"
if [[ -e "$ROOT_DIR/dist/CadLens.app" ]]; then
    mv "$ROOT_DIR/dist/CadLens.app" "$BACKUP_DIR/CadLens.app"
fi
mkdir -p "$ROOT_DIR/dist"
/usr/bin/ditto "$APP" "$ROOT_DIR/dist/CadLens.app"
CADLENS_MAC_ARCH_LABEL=macos12-universal \
    "$ROOT_DIR/script/package_macos.sh"
