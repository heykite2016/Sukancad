#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${CADLENS_WIN_BUILD_DIR:-$ROOT_DIR/build-win64-clang17}"
QT_WIN="${CADLENS_WIN_QT_ROOT:-$HOME/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64}"
MINGW_ROOT="${CADLENS_LLVM_MINGW_ROOT:-$HOME/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal}"
MAKENSIS="${CADLENS_MAKENSIS:-/tmp/nsis-3.11-src/build/urelease/makensis/makensis}"
NSISDIR="${CADLENS_NSISDIR:-/tmp/nsis-bin/nsis-3.11}"
PDFIUM_DIR="${CADLENS_PDFIUM_DIR:-$HOME/Library/Caches/CadLens/pdfium/chromium-8125-win-x64}"
VERSION="1.0.4"
PACKAGE_TAG="${CADLENS_PACKAGE_TAG:-windows10-x64}"
ZIP_OUT="$ROOT_DIR/dist/CadLens-${VERSION}-${PACKAGE_TAG}-portable.zip"
SETUP_OUT="$ROOT_DIR/dist/CadLens-${VERSION}-${PACKAGE_TAG}-setup.exe"

if [[ ! -f "$BUILD_DIR/CadLens.exe" ]]; then
    echo "Missing Windows executable in $BUILD_DIR" >&2
    exit 2
fi
if [[ ! -f "$QT_WIN/bin/Qt6Core.dll" ]]; then
    echo "Missing Windows Qt runtime in $QT_WIN" >&2
    exit 2
fi
if [[ ! -f "$PDFIUM_DIR/bin/pdfium.dll" ]]; then
    echo "Missing PDFium runtime in $PDFIUM_DIR" >&2
    exit 2
fi

SUBSYSTEM_MAJOR="$("$MINGW_ROOT/bin/llvm-readobj" \
    --file-headers "$BUILD_DIR/CadLens.exe" \
    | awk '/MajorSubsystemVersion:/ { print $2; exit }')"
if [[ "$SUBSYSTEM_MAJOR" != "6" ]]; then
    echo "Invalid MinGW PE subsystem version: $SUBSYSTEM_MAJOR" >&2
    echo "MinGW executables marked Windows 10.0 fail on Windows 11 with 0xc000007b." >&2
    exit 4
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
rm -f "$ZIP_OUT" "$SETUP_OUT"
STAGE="$WORK_DIR/CadLens"
mkdir -p "$STAGE/platforms" "$STAGE/iconengines" \
    "$STAGE/imageformats" "$STAGE/styles"

cp "$BUILD_DIR/CadLens.exe" "$STAGE/"
cp "$PDFIUM_DIR/bin/pdfium.dll" "$STAGE/"
for qt_dll in \
    Qt6Core.dll \
    Qt6Gui.dll \
    Qt6Widgets.dll \
    Qt6OpenGL.dll \
    Qt6OpenGLWidgets.dll \
    Qt6Svg.dll \
    Qt6Concurrent.dll \
    Qt6PrintSupport.dll; do
    cp "$QT_WIN/bin/$qt_dll" "$STAGE/"
done
cp "$MINGW_ROOT/x86_64-w64-mingw32/bin/libc++.dll" "$STAGE/"
cp "$MINGW_ROOT/x86_64-w64-mingw32/bin/libunwind.dll" "$STAGE/"
cp "$MINGW_ROOT/x86_64-w64-mingw32/bin/libwinpthread-1.dll" "$STAGE/"

cp "$QT_WIN/plugins/platforms/qwindows.dll" "$STAGE/platforms/"
cp "$QT_WIN/plugins/iconengines/qsvgicon.dll" "$STAGE/iconengines/"
cp "$QT_WIN"/plugins/imageformats/qgif.dll \
   "$QT_WIN"/plugins/imageformats/qico.dll \
   "$QT_WIN"/plugins/imageformats/qjpeg.dll \
   "$QT_WIN"/plugins/imageformats/qsvg.dll \
   "$STAGE/imageformats/"
cp "$QT_WIN/plugins/styles/qmodernwindowsstyle.dll" "$STAGE/styles/"
cp "$ROOT_DIR/resources/package/README.txt" "$STAGE/"

(
    cd "$WORK_DIR"
    /usr/bin/zip -q -r -9 "$ZIP_OUT" CadLens
)

if [[ "${CADLENS_PORTABLE_ONLY:-0}" == "1" ]]; then
    echo "$ZIP_OUT"
    exit 0
fi

if [[ ! -x "$MAKENSIS" || ! -d "$NSISDIR" ]]; then
    echo "Portable ZIP created: $ZIP_OUT"
    echo "NSIS compiler/data not found; setup EXE was not generated." >&2
    exit 3
fi

LC_ALL=en_US.UTF-8 LANG=en_US.UTF-8 NSISDIR="$NSISDIR" \
    "$MAKENSIS" -V4 -INPUTCHARSET UTF8 \
    -DSTAGE="$STAGE" \
    -DOUTFILE="$SETUP_OUT" \
    -DAPPICON="$ROOT_DIR/resources/windows/CadLens.ico" \
    "$ROOT_DIR/script/windows/CadLens.nsi" >/dev/null

echo "$ZIP_OUT"
echo "$SETUP_OUT"
