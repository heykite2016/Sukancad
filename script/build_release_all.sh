#!/usr/bin/env bash
# 一次产出 macOS + Windows 免安装发布包（两平台运行库全部随包，用户机器无需装任何依赖）。
#
# 用法：
#   ./script/build_release_all.sh            # 两个平台都出
#   ./script/build_release_all.sh macos      # 只出 macOS
#   ./script/build_release_all.sh windows    # 只出 Windows
#
# 依赖（首次需准备好的缓存）：
#   macOS Qt 6.8.3：$HOME/Qt/6.8.3/macos 或 $CADLENS_MAC_QT_ROOT
#   Windows Qt 6.8.3 llvm-mingw_64：~/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64
#   llvm-mingw 20231128 (Clang 17)：~/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal
#   libiconv（交叉编译静态库）：~/Library/Caches/CadLens/third-party/libiconv-win64
#   PDFium 8125 win-x64：~/Library/Caches/CadLens/pdfium/chromium-8125-win-x64
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MAC_QT="${CADLENS_MAC_QT_ROOT:-$HOME/Qt/6.8.3/macos}"
WIN_QT="${CADLENS_WIN_QT_ROOT:-$HOME/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64}"
MINGW="${CADLENS_LLVM_MINGW_ROOT:-$HOME/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal}"
ICONV="${CADLENS_ICONV_ROOT:-$HOME/Library/Caches/CadLens/third-party/libiconv-win64}"
TOOLS="$HOME/Library/Caches/CadLens/tools/bin"
export PATH="$TOOLS:$HOME/Library/Python/3.14/bin:$PATH"

TARGET="${1:-all}"

build_macos() {
    echo "==> macOS universal Release (deployment target 12.0)"
    # 必须显式指定 CMAKE_OSX_DEPLOYMENT_TARGET，否则新版 CMake 会默认成
    # 当前主机的系统版本，产出的包在老 Mac 上无法运行。
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-macos-release" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_OSX_ARCHITECTURES="x86_64;arm64" \
        -DCMAKE_OSX_DEPLOYMENT_TARGET=12.0 \
        -DCMAKE_PREFIX_PATH="$MAC_QT" \
        -DBUILD_TESTING=OFF
    cmake --build "$ROOT_DIR/build-macos-release" -j"$(sysctl -n hw.ncpu)"

    local app="$ROOT_DIR/build-macos-release/CadLens.app"
    "$MAC_QT/bin/macdeployqt" "$app" -always-overwrite
    "$ROOT_DIR/script/generate_app_icon.sh" \
        "$ROOT_DIR/resources/icons/logo.svg" \
        "$ROOT_DIR/build-macos-release/CadLens.icns"
    cp "$ROOT_DIR/build-macos-release/CadLens.icns" \
        "$app/Contents/Resources/CadLens.icns"
    /usr/libexec/PlistBuddy -c "Set :CFBundleIconFile CadLens.icns" \
        "$app/Contents/Info.plist" 2>/dev/null \
        || /usr/libexec/PlistBuddy -c "Add :CFBundleIconFile string CadLens.icns" \
            "$app/Contents/Info.plist"
    /usr/bin/strip -x "$app/Contents/MacOS/CadLens"
    /usr/bin/codesign --force --deep --options runtime \
        --entitlements "$ROOT_DIR/script/CadLens.entitlements" \
        --sign - "$app"
    /usr/bin/codesign --verify --deep --strict "$app"

    rm -rf "$ROOT_DIR/dist/CadLens.app"
    mkdir -p "$ROOT_DIR/dist"
    /usr/bin/ditto "$app" "$ROOT_DIR/dist/CadLens.app"
    CADLENS_MAC_ARCH_LABEL=macos12-universal \
        "$ROOT_DIR/script/package_macos.sh"
}

build_windows() {
    echo "==> Windows 10+ x64 Release (llvm-mingw Clang 17)"
    for path in "$WIN_QT/bin/Qt6Core.dll" "$MINGW/bin/x86_64-w64-mingw32-clang++" \
                "$ICONV/lib/libiconv.a"; do
        [[ -e "$path" ]] || { echo "缺少交叉编译依赖：$path" >&2; exit 2; }
    done
    export CADLENS_LLVM_MINGW_ROOT="$MINGW"
    export CADLENS_WIN_QT_ROOT="$WIN_QT"
    # QT_HOST_PATH 指向 macOS 版 Qt：Windows 包里的 moc.exe 无法在 macOS 上执行。
    cmake -S "$ROOT_DIR" -B "$ROOT_DIR/build-win64-release" -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_TOOLCHAIN_FILE="$ROOT_DIR/script/toolchains/llvm-mingw-windows.cmake" \
        -DCMAKE_PREFIX_PATH="$WIN_QT" \
        -DQT_HOST_PATH="$MAC_QT" \
        -DIconv_INCLUDE_DIR="$ICONV/include" \
        -DIconv_LIBRARY="$ICONV/lib/libiconv.a" \
        -DIconv_IS_BUILT_IN=OFF \
        -DBUILD_TESTING=OFF
    cmake --build "$ROOT_DIR/build-win64-release" -j"$(sysctl -n hw.ncpu)"

    CADLENS_WIN_BUILD_DIR="$ROOT_DIR/build-win64-release" \
        CADLENS_PORTABLE_ONLY=1 \
        "$ROOT_DIR/script/package_windows.sh"
}

case "$TARGET" in
    macos)   build_macos ;;
    windows) build_windows ;;
    all)     build_macos; build_windows ;;
    *)       echo "用法: $0 [all|macos|windows]" >&2; exit 2 ;;
esac

VERSION="$(sed -n 's/^project(CadLens VERSION \([0-9.]*\).*/\1/p' "$ROOT_DIR/CMakeLists.txt")"
(
    cd "$ROOT_DIR/dist"
    shasum -a 256 CadLens-"$VERSION"-*.dmg CadLens-"$VERSION"-*.pkg \
        CadLens-"$VERSION"-*portable.zip 2>/dev/null \
        | tee "Sukancad-$VERSION-SHA256SUMS.txt"
)
echo "==> 完成，产物在 $ROOT_DIR/dist"
