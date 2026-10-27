#!/usr/bin/env bash
set -euo pipefail

MODE="${1:-run}"
DRAWING="${2:-}"
APP_NAME="CadLens"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DIST_DIR="$ROOT_DIR/dist"
APP_DIST="$DIST_DIR/$APP_NAME.app"
QT_CACHE="$HOME/Library/Caches/CadLens/Qt/6.8.3/macos"
if [[ -z "${QT_ROOT:-}" ]]; then
    if [[ -d "$QT_CACHE" ]]; then
        QT_ROOT="$QT_CACHE"
    else
        QT_ROOT="$ROOT_DIR/.qt/6.8.3/macos"
    fi
fi
APP_BUNDLE="$BUILD_DIR/$APP_NAME.app"
APP_BINARY="$APP_BUNDLE/Contents/MacOS/$APP_NAME"
APP_ICON="$BUILD_DIR/CadLens.icns"
export PATH="$HOME/Library/Python/3.14/bin:$PATH"

pkill -x "$APP_NAME" >/dev/null 2>&1 || true

if ! command -v cmake >/dev/null 2>&1; then
    echo "cmake not found. Install cmake and Ninja first." >&2
    exit 1
fi

if [[ ! -f "$BUILD_DIR/CMakeCache.txt" ]]; then
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" -G Ninja \
        -DCMAKE_BUILD_TYPE=RelWithDebInfo \
        -DCMAKE_PREFIX_PATH="$QT_ROOT" \
        -DBUILD_TESTING=ON
else
    cmake -S "$ROOT_DIR" -B "$BUILD_DIR" \
        -DCMAKE_PREFIX_PATH="$QT_ROOT" \
        -DBUILD_TESTING=ON >/dev/null
fi

# macdeployqt rewrites the bundle binary. Remove only the generated executable
# so Ninja always relinks the current source before deployment.
rm -f "$APP_BINARY"
cmake --build "$BUILD_DIR" -j 8

if [[ ! -f "$APP_ICON" \
      || "$ROOT_DIR/resources/icons/logo.svg" -nt "$APP_ICON" ]]; then
    "$ROOT_DIR/script/generate_app_icon.sh" \
        "$ROOT_DIR/resources/icons/logo.svg" "$APP_ICON"
fi
mkdir -p "$APP_BUNDLE/Contents/Resources"
cp "$APP_ICON" "$APP_BUNDLE/Contents/Resources/CadLens.icns"
if /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile CadLens.icns" \
    "$APP_BUNDLE/Contents/Info.plist" >/dev/null 2>&1; then
    :
else
    /usr/libexec/PlistBuddy -c \
        "Add :CFBundleIconFile string CadLens.icns" \
        "$APP_BUNDLE/Contents/Info.plist"
fi

if [[ -x "$QT_ROOT/bin/macdeployqt" ]]; then
    "$QT_ROOT/bin/macdeployqt" "$APP_BUNDLE" -always-overwrite >/dev/null
    # Release hardening: remove local/debug symbols before signing. This does
    # not replace code signing or notarization, but makes static analysis and
    # symbol recovery materially harder.
    /usr/bin/strip -x "$APP_BUNDLE/Contents/MacOS/$APP_NAME"
    /usr/bin/codesign --force --deep --options runtime --sign - \
        --entitlements "$ROOT_DIR/script/CadLens.entitlements" \
        "$APP_BUNDLE" >/dev/null
    /usr/bin/codesign --verify --deep --strict "$APP_BUNDLE"
    mkdir -p "$DIST_DIR"
    ditto "$APP_BUNDLE" "$APP_DIST"
    if ! cmp -s "$APP_BINARY" "$APP_DIST/Contents/MacOS/$APP_NAME"; then
        echo "Deployment verification failed: dist binary is stale." >&2
        exit 1
    fi
fi

open_app() {
    if [[ -n "$DRAWING" ]]; then
        /usr/bin/open -n "$APP_BUNDLE" --args "$DRAWING"
    else
        /usr/bin/open -n "$APP_BUNDLE"
    fi
}

case "$MODE" in
    run|--run)
        open_app
        ;;
    --debug|debug)
        lldb -- "$APP_BINARY"
        ;;
    --logs|logs)
        open_app
        /usr/bin/log stream --info --style compact \
            --predicate "process == \"$APP_NAME\""
        ;;
    --verify|verify)
        open_app
        sleep 2
        pgrep -x "$APP_NAME" >/dev/null
        echo "$APP_NAME is running"
        ;;
    *)
        echo "usage: $0 [run|--debug|--logs|--verify] [DWG-or-DXF]" >&2
        exit 2
        ;;
esac
