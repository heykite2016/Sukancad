速看CAD 2.0.2
================

支持系统
- Windows 10 或更高版本，x64
- macOS 12 或更高版本，Intel x64 与 Apple Silicon arm64

安装
- macOS：安装 PKG，或打开 DMG 后将 CadLens.app 复制到 Applications。
- Windows：解压 CadLens-2.0.2-windows10-x64-portable.zip，运行
  CadLens/CadLens.exe。无需另装 Qt 或其他运行库。

当前功能
- 只读打开 DWG、DXF 和多页 PDF。
- DWG/DXF 模型空间与布局空间、图层显隐、视口、外部参照和对象捕捉。
- 读取文字、块属性、ACI/RGB 颜色、线宽和 HATCH。
- 测量：对齐、线性、连续、批量、矩形、多边形、圆面积、填充面积、
  侧面积、半径、角度和弧长。
- 标注：实时预览、文字注释、比例校准、样式、显隐、删除、撤销和重做。
- 输出：SVG、多页打印、打印到 PDF 和布局图框批量打印。
- 原始图纸不会被写回。

快速使用
1. 打开图纸。
2. 使用底部标签切换模型、布局或 PDF 页面。
3. 从主工具栏打开“测量工具”。
4. 从“设置”调整标注样式、单位、精度和十字光标。
5. 标注自动保存；Ctrl+S 可立即保存。

本版本不提供坐标标注、直径标注、点到线距离、定距标注和偏移面积。

数据位置
- 正式标注：当前用户应用数据目录的 annotations 子目录。
- 自动恢复、解析缓存、最近记录和缩略图：当前用户缓存目录。
- 最近图纸默认保留 12 条；解析缓存和未保存自动副本保留 30 天。
- 详细路径见源码仓库 DATA_STORAGE.md。

随包运行库
- Qt 6.8.3
- PDFium 158.0.8125.0
- LibreDWG、libdxfrw、GNU libiconv
- Osifont、Noto Sans CJK SC

许可与签名
- 项目许可：GPL-3.0-or-later。
- 完整许可与第三方声明见源码仓库 LICENSE、NOTICE.md 和 third_party/。
- 校验值见 Sukancad-2.0.2-SHA256SUMS.txt。
- macOS 为 ad-hoc 签名，未做 Apple Developer ID 公证。
- Windows 未使用商业代码签名证书。
