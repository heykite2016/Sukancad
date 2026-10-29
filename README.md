# 速看CAD

面向工程施工看图的 native DWG/DXF 测量与矢量标注工具，使用 C、C++20 和 Qt 6 构建。

当前发布版本：`1.0.4`。支持 Windows 10+ x64；macOS 12+ Universal 2（Intel 与 Apple Silicon）。

## 下载安装

- **macOS 12+（Intel / Apple Silicon）**：
  [PKG 安装包](https://github.com/heykite2016/Sukancad/releases/download/v1.0.4/CadLens-1.0.4-macos12-universal.pkg)
  · [DMG 安装包](https://github.com/heykite2016/Sukancad/releases/download/v1.0.4/CadLens-1.0.4-macos12-universal.dmg)
- **Windows 10+ x64**：
  [安装程序](https://github.com/heykite2016/Sukancad/releases/download/v1.0.4/CadLens-1.0.4-windows10-x64-setup.exe)
  · [便携版 ZIP](https://github.com/heykite2016/Sukancad/releases/download/v1.0.4/CadLens-1.0.4-windows10-x64-portable.zip)
- [查看全部发布文件及源码包](https://github.com/heykite2016/Sukancad/releases/tag/v1.0.4)

macOS 当前采用 ad-hoc 签名，尚未完成 Apple Developer ID 签名及公证。

## 已实现

- 启动工作区：类似图像软件的欢迎页，使用恒定饱和度与明度、仅动画色相的 HSL 霓虹流光曲线；主光从左后方 45° 柔和漫射，采用 40% 透明区、20% 指数过渡带、40% 光核，明暗交界带严格占线宽 1/6，并叠加多层柔光、弥散和微弱光学噪点；右上角“动效”开关可切换动态或静态并记住选择。提供独立打开入口和最近打开的 10 张图纸缩略图。
- 多图纸会话：工具栏下方左对齐 CAD 风格图纸标签，支持关闭；每张图纸保留独立模型、布局、缩放、测量和撤销状态。
- 后台准备：DWG/DXF 解析、布局视口展开、空间索引和最近图纸缩略图均在后台线程完成，主线程只提交已准备好的画布。
- 读取缓存：首次读取后按文件路径、大小和修改时间缓存已解析场景；文件未变化时跳过完整 DWG 解码，缓存由系统统一清理。
- 直接读取 DWG/DXF/PDF：DWG 主路径使用 `LibreDWG` 的块头和布局对象，按真实空间归属展开模型与布局；DXF 使用 `libdxfrw`；PDF 在 macOS 使用 PDFKit，在 Windows 使用随安装包部署的 PDFium。
- 外部参照：识别 DWG 中的 XREF；文件缺失或路径失效时可在打开流程中定位原图，选择结果按主图记忆并自动重载，外部参照按原模型、布局和插入变换绘制。
- OpenGL 硬件加速画布：启动时启用桌面 OpenGL、多画布共享上下文和双缓冲；按 CAD 默认关闭线宽渲染，统一细线显示；8x MSAA、可见区域裁剪、空间网格索引、按颜色合批；布局渲染不再截断可见图元集合。
- 小尺寸线条：静止视图保留亚像素线段，仅剔除真正零长度边；低缩放下不会因 1 像素阈值隐藏短线。
- 图层：读取 CAD 原始 ACI/RGB 颜色、线宽和显示状态；图层面板与图元使用同一颜色，左侧可逐层显隐。
- 空间切换与视图：按 LibreDWG 布局对象读取纸空间图元和真实 `VIEWCTR`、`VIEWSIZE`、`VIEWTWIST`，排除整页控制视口；支持布局中的尺寸块、引线、箭头和块属性。按“显示全部 DWG 内容”的要求，读取到的逐视口冻结信息默认不用于删除图元。CAD `POINT` 通常是尺寸定义点或块控制点，默认不作为图纸线条显示。
- 填充：CAD 填充默认只绘制边界线，仅明确的墙体实心填充保留填色；矩形和多边形测量均为线框标注。
- 对象捕捉：端点、中点、交点、垂足、圆心、象限点、最近点。
- 点到直线距离：先选点，再单击已有直线，自动求垂足并在点与垂足间绘制尺寸线。
- CAD 交互：选择与测量状态均显示自绘 CAD 十字线，不叠加原生十字光标；拖动图纸时改为手形光标并隐藏十字线。栅格默认开启，`F3` 对象捕捉、`F7` 栅格、`F8` 正交，双击中键显示全图。
- 测量工具栏：默认关闭，可从视图菜单打开后停靠左侧、右侧或直接拖动；显示状态和停靠位置自动保存。
- 尺寸与面积：对齐、水平/垂直线性、连续总长、批量、矩形周长与面积、闭合多边形、圆、直径、角度、弧长、坐标、点到直线距离。连续测量逐段在线段和圆弧上显示长度并单独放置总长，圆弧本体保留绘制；批量尺寸朝鼠标方向布置。圆测量只显示半径、面积和周长，不标注圆心位置；直径测量绘制完整圆、直径线和圆心标记；连续、面积和半径尺寸直接绘制在对应边线或半径线上，文字覆盖区自动断线，线长不足时自动改用引线。
- 工程辅助测量：定距、填充面积、侧面积、偏移面积、已知长度反算比例。侧面积支持单段或连续多段线段、圆弧，逐段标注长度并汇总总长与侧面积。
- 标注：矢量文字、引线、箭头和尺寸线；鼠标第三点决定标注位置；可拖动、编辑备注、显隐、定位、撤销、重做。
- 标注放置：尺寸文字能落在对应尺寸线或圆弧上时直接标注；文字过长、尺寸线不足或标注位置离开图元时，自动改用 CAD 引线标注。
- 删除：删除工具支持点选和框选批量删除，删除后保持删除模式；删除全部标注确认或取消后返回选择工具。
- 设置：长度、面积、坐标分别设置单位与精度；长度默认精确到 mm，面积默认以 m² 换算。尺寸、面积和周长标注均只显示数值，面积和周长使用“面积：/周长：”，默认标注颜色为绿色且可单独设置。默认主题为 CAD 深色，默认十字光标为白色、屏幕 100% 大小、1.25 px 线宽。
- 标注样式：提供 ISO 标准、建筑斜线、机械 ISO-25、工程开口箭头、小数圆点和紧凑端线六种常用样式。尺寸文字以图纸现有尺寸字高为基准，随图纸缩放同步变化。
- 保存：测量修改、正式标注、比例和最近打开记录统一写入系统缓存目录，不修改图纸所在目录；由操作系统管理缓存清理。
- 矢量输出：将当前视图、草图文字和测量标注导出为 SVG。
- 打印：在图纸上框选打印范围，支持 600/1200/2400 DPI 三档分辨率，可添加多页并逐页框选；框选外保持白色，可选择 CAD 原色或全黑模式。
- 字体与图标：工具栏和产品标识均为 SVG；制图文字使用随应用打包的 Osifont 与 Noto Sans CJK SC，并通过 `QPainterPath` 绘制轮廓。
- 帮助与界面：`F1` 打开白底使用帮助；所有应用弹窗和状态栏采用白底；启动页未打开图纸时工具栏保持禁用。
- 应用图标：1024×1024 工程图纸/测量透镜主题矢量标志，构建时生成完整 macOS ICNS 图标集并写入应用包。
- 文件关联：可在设置中开关 DWG、DXF 的默认打开方式；系统“打开方式”列表包含 DWG、DXF、PDF，关联后的 CAD 文件使用速看CAD专属图标。

所有箭头类标注统一采用 CAD 终端符号规则：箭头尖端接触目标线或尺寸界线，箭身沿尺寸线/引线向内展开。黑色或接近黑色的 CAD 图元在黑色画布上会自动显示为白色，其他颜色保持 CAD 原色。

## 构建

完整读图、布局、交互和性能验收流程见 [WORKFLOW.md](WORKFLOW.md)，性能取舍见 [PERFORMANCE.md](PERFORMANCE.md)。

### 发布加固

发布构建会启用 LTO、隐藏内部 C/C++ 可见性、死代码裁剪、栈保护，并将最终可执行文件的导出符号收窄到入口点；部署阶段移除本地/调试符号后启用 macOS Hardened Runtime。当前本地包使用 ad-hoc 签名，因此包含 Qt 库加载所需 entitlement；正式对外分发时应替换为 Developer ID 签名并完成 Apple 公证。可执行程序必须能被系统解密执行，因此不存在“真正无法逆向”的纯本地方案。

Qt 构建依赖保存在 `~/Library/Caches/CadLens/Qt/6.8.3/macos`，不占用项目目录。构建需要 CMake、Ninja、Apple Clang 与 Qt 6.6+。

```bash
./script/build_and_run.sh
```

最终应用输出在 `dist/CadLens.app`。

### 平台安装包

```bash
./script/build_macos_universal.sh
```

macOS 产物为 macOS 12+、Intel + Apple Silicon Universal 2 的 DMG 与 PKG。

Windows 10+ x64 可在 macOS 上使用 Qt 6.8.3 `win64_llvm_mingw`、llvm-mingw 20231128（Clang 17）和静态 libiconv 交叉编译。Qt 6.8.3 的 Windows 运行库按 Clang 17 ABI 构建，不能混用其他版本的 `libc++.dll`；工具链配置会拒绝非 Clang 17 编译器：

```bash
export CADLENS_LLVM_MINGW_ROOT="$HOME/Library/Caches/CadLens/toolchains/llvm-mingw-20231128-ucrt-macos-universal"
export CADLENS_WIN_QT_ROOT="$HOME/Library/Caches/CadLens/Qt/6.8.3/6.8.3/llvm-mingw_64"
export CADLENS_ICONV_ROOT="$HOME/Library/Caches/CadLens/third-party/libiconv-win64"
export CADLENS_PDFIUM_DIR="$HOME/Library/Caches/CadLens/pdfium/chromium-8125-win-x64"

cmake -S . -B build-win64-clang17 -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE="$PWD/script/toolchains/llvm-mingw-windows.cmake" \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_PREFIX_PATH="$CADLENS_WIN_QT_ROOT;$CADLENS_ICONV_ROOT" \
  -DQT_HOST_PATH="$HOME/Library/Caches/CadLens/Qt/6.8.3/macos" \
  -DIconv_INCLUDE_DIR="$CADLENS_ICONV_ROOT/include" \
  -DIconv_LIBRARY="$CADLENS_ICONV_ROOT/lib/libiconv.a" \
  -DBUILD_TESTING=OFF
cmake --build build-win64-clang17 -j8
./script/package_windows.sh
```

Windows 产物包括完整 Qt 运行库、PDFium 和 NSIS 安装器。当前本地包没有商业代码签名证书。

打开指定图纸：

```bash
./script/build_and_run.sh run "/path/to/drawing.dwg"
```

验证进程：

```bash
./script/build_and_run.sh --verify
```

读取与渲染基线验收：

```bash
./script/validate_dwg.sh "/path/to/drawing.dwg"
```

验收会检查 LibreDWG 后端、模型实体、原始图层颜色、有限坐标、布局和模型视口、文字格式控制码残留。模型与布局读取基线未通过时，不继续增加测量功能。

可生成固定尺寸的模型或布局截图，用于与 CAD 快速看图逐页比对：

```bash
./dist/CadLens.app/Contents/MacOS/CadLens \
  --snapshot /tmp/layout1.png --snapshot-space 布局1 \
  --snapshot-size 17000x1000 "/path/to/drawing.dwg"
```

`--inspect-entities` 会输出模型和布局图元的句柄、图层、范围、闭合与填充状态以及坐标点，用于回归定位。

## 交互

- `V`：选择与拖动标注；在空白图纸上按住左键拖动视图。
- 任意测量工具中按住鼠标左键拖动即可平移图纸；滚轮始终以鼠标当前位置为中心缩放。
- `A`：对齐尺寸；`L`：水平/垂直线性尺寸。
- `C`：连续测量；依次单击路径节点，右键、`Enter`、双击或工具栏“完成当前测量”结束路径，再单击图纸放置总长标注。
- 连续、批量、多边形和偏移面积测量可按 `Esc` 在当前最后点结束路径。
- `P`：闭合多边形；单击起点或双击闭合。
- 中键拖动始终可平移，滚轮缩放。
- `F3` 对象捕捉、`F7` 栅格（默认开启）、`F8` 正交；双击中键显示全图。
- `Z` 或工具栏“窗口缩放”：按住左键框出矩形，松手后放大该区域。
- 关闭图纸标签前自动保存测量；关闭最后一个标签后返回启动页。
- `Esc` 取消当前草图；`Delete` 删除当前标注。

## 源码结构

- `src/core`：纯 C 几何计算与测试。
- `src/CadModel.*`：DWG/DXF 解析、实体归一化、图层与单位。
- `src/SpatialIndex.*`：大图空间索引。
- `src/SnapEngine.*`：对象捕捉。
- `src/Measurements.*`：测量结果、单位换算和标注持久化。
- `src/CadView.*`：OpenGL 画布、矢量绘制与交互。
- `src/MainWindow.*`：工业风格桌面界面与管理面板。
- `third_party/libdxfrw`：DWG/DXF 开源解析内核。
- `third_party/libredwg`：DWG 主解析内核和布局/视口读取。

## 第三方许可

`libdxfrw` 使用 GPL-2.0-or-later，`LibreDWG` 使用 GPL-3.0-or-later。Osifont 使用 GPL/LGPL 加字体例外。Noto Sans CJK SC 使用 SIL Open Font License 1.1。具体许可见 `third_party/libdxfrw/COPYING`、`third_party/libredwg/COPYING` 与 `resources/fonts/*license*`。

速看CAD 项目自身按 GPL-3.0-or-later 发布，完整文本和第三方声明见
`LICENSE` 与 `NOTICE.md`。
