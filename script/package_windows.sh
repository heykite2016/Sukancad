#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${CADLENS_WIN_BUILD_DIR:-$ROOT_DIR/build-win64}"
QT_WIN="${CADLENS_WIN_QT_ROOT:-$HOME/Library/Caches/CadLens/Qt/6.8.3/windows-llvm-mingw/6.8.3/llvm-mingw_64}"
MINGW_ROOT="${CADLENS_LLVM_MINGW_ROOT:-$HOME/Library/Caches/CadLens/toolchains/llvm-mingw-20261020-ucrt-macos-universal}"
MAKENSIS="${CADLENS_MAKENSIS:-/tmp/nsis-3.11-src/build/urelease/makensis/makensis}"
NSISDIR="${CADLENS_NSISDIR:-/tmp/nsis-bin/nsis-3.11}"
VERSION="1.0.1"
ZIP_OUT="$ROOT_DIR/dist/CadLens-${VERSION}-windows10-x64-portable.zip"
SETUP_OUT="$ROOT_DIR/dist/CadLens-${VERSION}-windows10-x64-setup.exe"

if [[ ! -f "$BUILD_DIR/CadLens.exe" ]]; then
    echo "Missing Windows executable in $BUILD_DIR" >&2
    exit 2
fi
if [[ ! -f "$QT_WIN/bin/Qt6Core.dll" ]]; then
    echo "Missing Windows Qt runtime in $QT_WIN" >&2
    exit 2
fi

WORK_DIR="$(mktemp -d)"
trap 'rm -rf "$WORK_DIR"' EXIT
STAGE="$WORK_DIR/CadLens"
mkdir -p "$STAGE/platforms" "$STAGE/iconengines" \
    "$STAGE/imageformats" "$STAGE/styles"

cp "$BUILD_DIR/CadLens.exe" "$STAGE/"
cp "$QT_WIN"/bin/Qt6*.dll "$STAGE/"
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

if [[ ! -x "$MAKENSIS" || ! -d "$NSISDIR" ]]; then
    echo "Portable ZIP created: $ZIP_OUT"
    echo "NSIS compiler/data not found; setup EXE was not generated." >&2
    exit 3
fi

NSISDIR="$NSISDIR" "$MAKENSIS" -V2 \
    -DSTAGE="$STAGE" \
    -DOUTFILE="$SETUP_OUT" \
    -DAPPICON="$ROOT_DIR/resources/windows/CadLens.ico" \
    "$ROOT_DIR/script/windows/CadLens.nsi"

echo "$ZIP_OUT"
echo "$SETUP_OUT"
