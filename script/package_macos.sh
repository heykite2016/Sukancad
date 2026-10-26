#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="${CADLENS_MAC_APP:-$ROOT_DIR/dist/CadLens.app}"
VERSION="1.0.0"

if [[ ! -d "$APP" ]]; then
    echo "Missing $APP. Build the macOS app first." >&2
    exit 2
fi
if [[ -n "${CADLENS_MAC_ARCH_LABEL:-}" ]]; then
    ARCH="$CADLENS_MAC_ARCH_LABEL"
else
    _ARCHES="$(/usr/bin/lipo -info "$APP/Contents/MacOS/CadLens")"
    if [[ "$_ARCHES" == *"arm64 x86_64"* \
          || "$_ARCHES" == *"x86_64 arm64"* ]]; then
        ARCH="universal"
    else
        ARCH="x64"
    fi
fi
DMG="$ROOT_DIR/dist/CadLens-${VERSION}-macos12-${ARCH}.dmg"
PKG="$ROOT_DIR/dist/CadLens-${VERSION}-macos12-${ARCH}.pkg"

/usr/bin/codesign --verify --deep --strict "$APP"
MIN_OS="$(
    /usr/bin/otool -l "$APP/Contents/MacOS/CadLens" \
        | awk '/LC_BUILD_VERSION/{found=1} found && /minos/{print $2; exit}'
)"
if [[ "$MIN_OS" != "12.0" && "$MIN_OS" != "12" ]]; then
    echo "Unexpected macOS deployment target: ${MIN_OS:-unknown}" >&2
    exit 1
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
DMG_ROOT="$WORK_DIR/dmg-root"
PKG_ROOT="$WORK_DIR/pkg-root"
mkdir -p "$DMG_ROOT" "$PKG_ROOT/Applications"
/usr/bin/ditto "$APP" "$DMG_ROOT/CadLens.app"
/usr/bin/ditto "$APP" "$PKG_ROOT/Applications/CadLens.app"
ln -s /Applications "$DMG_ROOT/Applications"
/usr/bin/ditto \
    "$ROOT_DIR/resources/package/README.txt" \
    "$DMG_ROOT/README.txt"

COMPONENT_PLIST="$WORK_DIR/components.plist"
/usr/bin/pkgbuild \
    --analyze \
    --root "$PKG_ROOT" \
    "$COMPONENT_PLIST" >/dev/null
if ! /usr/libexec/PlistBuddy \
    -c "Set :0:BundleIsRelocatable false" \
    "$COMPONENT_PLIST"; then
    echo "Failed to disable macOS app relocation." >&2
    exit 1
fi

/usr/bin/hdiutil create \
    -volname "CAD Lens" \
    -srcfolder "$DMG_ROOT" \
    -format UDZO \
    -ov \
    "$DMG" >/dev/null
/usr/bin/pkgbuild \
    --root "$PKG_ROOT" \
    --component-plist "$COMPONENT_PLIST" \
    --scripts "$ROOT_DIR/script/macos" \
    --identifier "com.cadlens.desktop" \
    --version "$VERSION" \
    --install-location "/" \
    "$PKG" >/dev/null

echo "$DMG"
echo "$PKG"
