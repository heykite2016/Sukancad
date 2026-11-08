# Android 移植说明

当前 Android 版本为 `2.0.1`，是原生 Kotlin/Canvas 应用，不复用桌面端 Qt UI。

## 当前已移植

- LibreDWG DWG 读取。
- LibreDWG DXF 读取，使用独立的 `dxf_read_file` 入口。
- 模型空间、布局、视口、图层、外部参照和块属性导出。
- INSERT/MINSERT 变换、SPLINE、2D/3D 多段线、HATCH、TEXT/MTEXT/ATTRIB。
- SKDW 二进制缓存。
- Android Canvas 平移、缩放、图层显隐、文字查找和布局切换。
- 长度、连续、面积、角度、弧长、半径和坐标测量。
- 文字、直线、云线、引线、编号、照片和本地标注保存。
- DWG/DXF 与 ZIP/RAR 图纸库导入。
- 与桌面端共享 `src/core/cad_geometry.c`：面积、周长、角度、圆和弧长计算。

## 当前未移植

- PDF 页面渲染与测量。
- 桌面端 `2.0.1` 的完整 UI、测量面板和标注样式系统。
- x86_64 Android ABI；当前只构建 `arm64-v8a`。
- 正式发布签名；当前 Release APK 使用 debug keystore。

Android 端不能宣称与桌面端功能完全一致。

## 开发环境

已使用环境：

```text
ANDROID_HOME=/Users/mr.wangjia/android-build/android-sdk
ANDROID_NDK_HOME=/Users/mr.wangjia/android-build/android-sdk/ndk/26.3.11579264
SDK CMake=/Users/mr.wangjia/android-build/android-sdk/cmake/3.22.1
JDK 21
minSdk 24
compileSdk/targetSdk 34
```

这些变量已写入 `~/.zshrc`。其他机器可设置自己的 SDK/NDK 路径。

## 构建

在仓库根目录：

```bash
./script/build_android.sh debug
./script/build_android.sh release
```

产物：

```text
dist/CadLens-2.0.1-android-arm64-debug.apk
dist/CadLens-2.0.1-android-arm64.apk
```

直接使用 Gradle：

```bash
cd android
./gradlew :app:assembleDebug
./gradlew :app:assembleRelease
```

## 安装与无线调试

```bash
adb devices -l
adb install -r dist/CadLens-2.0.1-android-arm64.apk
```

部分 OPPO/OnePlus 设备首次安装会显示系统安装确认页，需要点击“继续安装”。

Gradle 测试安装若被系统安装向导阻断，可手动安装应用与测试 APK，然后执行：

```bash
adb shell am instrument -w -r \
  -e class com.sukancad.cad.CorePortTest \
  com.sukancad.cad.test/androidx.test.runner.AndroidJUnitRunner
```

## 自动化测试

```bash
cd android
./gradlew :app:connectedDebugAndroidTest
```

当前设备测试：

- `sharedGeometryMatchesDesktopCore`：共享面积、周长、角度、圆和弧长。
- `parsesDxfThroughNativePipeline`：DXF → LibreDWG → SKDW → Kotlin 文档模型。

已在 Android 平板 `OPD2515` 上通过 `2/2`。

## 后续工作

1. 使用 Android `PdfRenderer` 增加 PDF 多页背景、测量和打印。
2. 将桌面端标注样式与测量交互逐项迁移。
3. 增加 Android x86_64 或模拟器 ABI。
4. 配置正式 keystore 和 AAB 发布。
5. 建立 Android CI，至少执行 assembleDebug、Lint 和设备测试。
