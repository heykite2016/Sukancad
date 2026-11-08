# 构建与依赖

本文记录速看CAD `2.0.3` 已验证的构建方式。项目主要开发和发布环境为 macOS；Windows x64 通过 llvm-mingw 交叉编译。

## 工具链

已验证环境：

- CMake 3.28 或更高版本
- Ninja
- Apple Clang / Command Line Tools
- Qt 6.8.3 macOS 版
- PDFium `158.0.8125.0`
- C++20

项目不会把 Qt 和 PDFium 安装到仓库目录。默认缓存路径如下：

```text
~/Library/Caches/CadLens/Qt/6.8.3/macos
~/Library/Caches/CadLens/pdfium/chromium-8125-mac-univ
```

可通过 `QT_ROOT` 和 `CADLENS_MAC_PDFIUM_ROOT` 覆盖。

## 获取 PDFium

macOS Universal 构建使用 Chromium 8125 的 macOS Universal 包：

```bash
pdfium_root="$HOME/Library/Caches/CadLens/pdfium/chromium-8125-mac-univ"
mkdir -p "$pdfium_root"
curl -fL \
  "https://github.com/bblanchon/pdfium-binaries/releases/download/chromium/8125/pdfium-mac-univ.tgz" \
  -o "$pdfium_root/pdfium-mac-univ.tgz"
tar -xzf "$pdfium_root/pdfium-mac-univ.tgz" -C "$pdfium_root"
```

构建脚本会把 `libpdfium.dylib` 复制到 App 的 `Contents/MacOS` 并在签名前纳入 App Bundle。

## 本地构建和运行

```bash
export DEVELOPER_DIR="${CADLENS_DEVELOPER_DIR:-/Library/Developer/CommandLineTools}"
export QT_ROOT="$HOME/Library/Caches/CadLens/Qt/6.8.3/macos"
export CADLENS_MAC_PDFIUM_ROOT="$HOME/Library/Caches/CadLens/pdfium/chromium-8125-mac-univ"

./script/build_and_run.sh
```

打开指定图纸：

```bash
./script/build_and_run.sh run "/path/to/drawing.dwg"
```

输出：

```text
build/CadLens.app
dist/CadLens.app
```

## 测试

```bash
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

当前测试目标：

- `cad_geometry_tests`：纯 C 几何算法。
- `cad_document_tests`：DWG/DXF/PDF 读取、缓存与数据模型。
- `cadview_interaction_tests`：画布、测量、标注、删除、打印和多页 PDF 交互。

提交修改前至少运行全部三个测试。

## 内存与生命周期检查

发布前使用 AddressSanitizer 构建并运行同一组测试：

```bash
cmake -S . -B build-asan -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_PREFIX_PATH="$HOME/Qt/6.8.3/macos" \
  -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -fno-omit-frame-pointer" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j"$(sysctl -n hw.ncpu)"
ASAN_OPTIONS=halt_on_error=1 \
  ctest --test-dir build-asan --output-on-failure
```

Apple Clang 的 ASan 不支持 `detect_leaks`；macOS 使用系统 `leaks` 检查
Objective-C/C++ 堆对象：

```bash
leaks --atExit -- build/cadview_interaction_tests
```

Qt 和系统框架可能保留一次性全局缓存；发布判断以项目对象、重复打开/关闭图纸和
重复打开设置窗口后的稳定内存为准。浮动 `QDockWidget`、`QToolBar` 必须随主窗口
关闭，画布缓存和打印预览同时受像素预算限制。

## macOS 发布包

```bash
export CADLENS_MAC_PDFIUM_ROOT="$HOME/Library/Caches/CadLens/pdfium/chromium-8125-mac-univ"
./script/build_macos_universal.sh
```

产物：

```text
dist/CadLens.app
dist/CadLens-2.0.3-macos12-universal.dmg
dist/CadLens-2.0.3-macos12-universal.pkg
```

macOS 最低版本为 `12.0`，架构为 `x86_64` 和 `arm64`。本地发布脚本使用 ad-hoc hardened runtime 签名，不能替代 Developer ID 签名和 Apple 公证。

## Windows x64 交叉编译

已验证工具链缓存：

```text
~/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64
~/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal
~/Library/Caches/CadLens/third-party/libiconv-win64
~/Library/Caches/CadLens/pdfium/chromium-8125-win-x64
```

Windows Qt DLL 必须来自同一 Qt 6.8.3 `win64_llvm_mingw` 安装，并与 llvm-mingw Clang 17 ABI 匹配。不要混用其他版本的 `libc++.dll`。

```bash
export CADLENS_LLVM_MINGW_ROOT="$HOME/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal"
export CADLENS_WIN_QT_ROOT="$HOME/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64"
export CADLENS_ICONV_ROOT="$HOME/Library/Caches/CadLens/third-party/libiconv-win64"
export CADLENS_PDFIUM_DIR="$HOME/Library/Caches/CadLens/pdfium/chromium-8125-win-x64"

cmake -S . -B build-win64-release -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/script/toolchains/llvm-mingw-windows.cmake" \
  -DCMAKE_PREFIX_PATH="$CADLENS_WIN_QT_ROOT" \
  -DQT_HOST_PATH="$HOME/Library/Caches/CadLens/Qt/6.8.3/macos" \
  -DIconv_INCLUDE_DIR="$CADLENS_ICONV_ROOT/include" \
  -DIconv_LIBRARY="$CADLENS_ICONV_ROOT/lib/libiconv.a" \
  -DIconv_IS_BUILT_IN=OFF \
  -DBUILD_TESTING=OFF
cmake --build build-win64-release -j"$(sysctl -n hw.ncpu)"

CADLENS_WIN_BUILD_DIR="$PWD/build-win64-release" \
  CADLENS_PORTABLE_ONLY=1 \
  ./script/package_windows.sh
```

产物为：

```text
dist/CadLens-2.0.3-windows10-x64-portable.zip
```

包内包含 Qt、PDFium 和 MinGW 运行库。LibreDWG、libdxfrw 和 libiconv 静态链接进程序。

## 两平台一次构建

依赖缓存准备完成后：

```bash
./script/build_release_all.sh all
```

该命令生成 DMG、PKG、Windows portable ZIP 和 SHA-256 清单。发布前检查项见 [RELEASE.md](RELEASE.md)。

## 命令行验收

读取统计：

```bash
dist/CadLens.app/Contents/MacOS/CadLens --inspect "/path/to/drawing.dwg"
```

输出图元信息：

```bash
dist/CadLens.app/Contents/MacOS/CadLens --inspect-entities "/path/to/drawing.dxf"
```

指定页面截图：

```bash
dist/CadLens.app/Contents/MacOS/CadLens \
  --snapshot /tmp/page2.png \
  --snapshot-space "第2页" \
  --snapshot-size 1600x900 \
  "/path/to/drawing.pdf"
```

## 已知构建限制

- Windows 路径目前由 macOS 交叉编译验证，尚未建立原生 Windows CI。
- macOS 本地包未使用 Developer ID，也未公证。
- Windows 包未使用商业代码签名证书。
- PDFium 与 Qt 属于独立的第三方运行时，必须在发布时保留对应许可和通知文件。
