# Android 移植说明

当前 Android 版本为 `2.0.2`，是原生 Kotlin/Canvas 应用，不复用桌面端 Qt UI。

## 当前已移植

- LibreDWG DWG 读取。
- LibreDWG DXF 读取，使用独立的 `dxf_read_file` 入口。
- 模型空间、布局、视口、图层、外部参照和块属性导出。
- INSERT/MINSERT 变换、SPLINE、2D/3D 多段线、HATCH、TEXT/MTEXT/ATTRIB。
- SKDW 二进制缓存。
- Android Canvas 平移、双指缩放、图层显隐、文字查找和布局切换。
- Canvas 自绘底栏和单排工具条，工具采用图标加文字，横向滑动，不弹多排大面板。
- 测量选点使用浮动虚拟鼠标：拖动鼠标本体移动准星，轻点鼠标确认；双指继续平移/缩放图纸。
- 测量：长度、连续、面积、角度、弧长、半径。
- 角度测量依次确认两条线段，再用虚拟鼠标确认文字落点。
- 点击尺寸线、尺寸文字、圆弧、圆点或引线等任意子图元会选中整条标注，并直接显示白色端点方框。
- 拖动白色方框实时重算尺寸；“删除”“完成”“取消”位于同一动作条，删除前二次确认。
- 自动比例校准：从原图数字尺寸与对应线段的一致性推断比例。
- 手动比例校准：拾取已知长度的两个端点后输入标注长度，与 PC 端流程一致。
- 标注只保留文字标注；“标注管理”统一查看、定位、移动或删除测量和文字标注。
- 已完成的测量和文字标注按模型/布局分别保存，重新打开图纸时恢复。
- DWG/DXF 与 ZIP/RAR 图纸库导入；微信等外部应用打开 ZIP 后自动解压并清理压缩包。
- 本机图纸列表左滑显示“删除”；最近列表左滑显示“移除”，只移除记录，不删除原图纸。
- 与桌面端共享 `src/core/cad_geometry.c`：面积、周长、角度、圆和弧长计算。

## 当前未移植

- PDF 页面渲染与测量。
- 桌面端全部高级标注样式和批量打印能力。
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
dist/CadLens-2.0.2-android-arm64-debug.apk
dist/CadLens-2.0.2-android-arm64.apk
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
adb install -r dist/CadLens-2.0.2-android-arm64.apk
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

当前设备测试共 `12/12`，覆盖：

- 共享面积、周长、角度、圆和弧长。
- DXF → LibreDWG → SKDW → Kotlin 文档模型。
- 虚拟鼠标拖动与轻点选点、控制柄拖动和已有测量端点修改。
- 点击尺寸线与尺寸文字选中整条标注，以及编辑后整条删除。
- 连续测量操作栏收尾、角度两点选线加文字落点。
- 自动比例校准样本识别。
- ZIP 解压、清理压缩包、缓存清理不删图。
- 图纸重命名/删除、最近列表移除不删原文件。

## 后续工作

1. 使用 Android `PdfRenderer` 增加 PDF 多页背景与测量。
2. 继续按触摸操作校正桌面端测量工具的移动端映射。
3. 增加 Android x86_64 或模拟器 ABI。
4. 配置正式 keystore 和 AAB 发布。
5. 建立 Android CI，至少执行 assembleDebug、Lint 和设备测试。
