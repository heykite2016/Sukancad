# CAD Lens 读图与渲染工作流

## 验收原则

1. DWG 是唯一几何来源。除字体轮廓、屏幕 LOD 和屏幕空间符号外，不创建图纸中不存在的实体。
2. `POINT` 作为尺寸定义点或块控制点时不显示；`WIPEOUT` 作为遮罩对象不绘制成线条。
3. 每个阶段先通过几何和图层账本，再做截图比对，最后才进入性能优化。

## 已完成检查

- [x] LibreDWG AC1018 解析，模型空间 28,092 个可见图元，97 个图层。
- [x] 块基点、负缩放、旋转、OCS、MINSERT、嵌套块转换。
- [x] 圆、弧、椭圆、样条的真实曲线显示。
- [x] `TEXT`、`MTEXT`、`ATTRIB`、`LEADER`、`TRACE`、`SOLID`、`HATCH` 边界。
- [x] 按 INSERT owner handle 索引 `ATTRIB`，不依赖可能为 0 的 `num_owned`。
- [x] 按 DWG 属性模式渲染：`ATTMODE=0` 全部隐藏、`1` 按各 `ATTRIB` 的可见标志、`2` 全部显示；验收输出全文件属性账本。
- [x] 当前样本全量属性对账：源文件 `6224` 个 `ATTRIB`（`780` 可见、`5444` 隐藏）；可达块实例展开 `4486` 个属性，隐藏泄漏为 `0`。未展开的 `1738` 个属性只存在于未实例化块定义中。
- [x] 布局1的12个模型视口及布局2的1个模型视口。
- [x] 读取每个模型视口的 `frozen_layers`，但默认不删除图元，保证所有 DWG 内容可见。
- [x] `TK`、`A-文字标注`、`A-尺寸` 核心图元逐层数量核对，无缺失几何类型。
- [x] 原生 LWPOLYLINE 闭合位、图框四边、引线箭头。
- [x] HATCH/MPOLYGON 开放边界与断开边组按原始端点拆分，禁止闭合或跨空档连线。
- [x] TEXT/ATTRIB/MTEXT 遵循 DWG `MIRRTEXT` 和块插入矩阵，反射块中的文字保持正确方向。
- [x] SOLID HATCH 按文件填充色成组绘制；图案 HATCH 按 DefLine、offset 和 dash 字段绘制。
- [x] GBK/GB18030 图层名恢复，例如 `EQUIP-通讯`。
- [x] 恢复 DWG `*Active` VPORT 的中心、宽度、高度和旋转。
- [x] 连续测量通过右键、Enter、双击或“完成当前测量”结束。
- [x] 连续/批量/多边形/偏移面积测量可按 Esc 在当前最后点结束。
- [x] 测量过程中左键拖动平移、滚轮以鼠标位置为中心缩放。
- [x] 测量十字线贯穿绘图视口并带中心拾取框；F3/F7/F8 和双击中键显示全图。
- [x] CAD 窗口缩放工具：左键框选区域后放大，并自动返回选择状态。
- [x] 矩形、多边形和面积偏移改为边界完成后选择标注位置。
- [x] 对齐、线性、连续、批量、矩形、多边形、圆、填充面积、侧面积、偏移面积、半径、直径、角度、弧长、坐标、点到直线和定距测量交互回归。
- [x] 标注显隐、文字注释、保存与重新载入回归。
- [x] 后台解析、空间索引、视口裁剪、样式合批、线段缓存和拖动位图缓存。
- [x] DWG/DXF 解析、全部布局视口展开、模型空间索引和最近图纸缩略图统一放到后台任务。
- [x] 屏幕空间 LOD：只省略零长度边；连续亚像素边保持原有点序，禁止跨段生成假桥线。
- [x] 缩放性能：图层 O(1) 查找、矢量字形轮廓缓存、文字按视口批量裁剪、未填充闭合轮廓合批、滚轮位图即时预览。
- [x] 比例校准、最近打开记录、标注和缩略图统一保存在系统缓存目录，由操作系统管理缓存清理。
- [x] 测量记录 400 ms 防抖自动保存，打开时优先加载自动副本；正式保存后只清理对应缓存自动副本。
- [x] 标注设置提供六种 CAD 样式图形勾选卡片，默认绿色；圆测量包含圆心、半径、面积和周长。
- [x] DWG 外部参照缺失时提供定位弹窗；所选路径按主图保存、自动重载，并展开到原模型或布局位置。
- [x] 启动页流光动效默认关闭，可在启动页开启并记忆选择。
- [x] 模型初始范围和显示全图覆盖全部有效图元，不再按空间密度排除独立的第二平面。
- [x] 设置页可分别开关 DWG、DXF 默认打开方式；Windows 安装器和 macOS 应用包注册 DWG、DXF、PDF“打开方式”项及 CAD 文件图标。
- [x] 布局中 `FIT`/`ALIGNED` 多行文字的对齐点与插入点同时应用布局变换，修复轴号被绘制为贯穿视口的宽线。

## 固定验收命令

```bash
./script/build_and_run.sh --verify
./script/validate_dwg.sh "/path/to/drawing.dwg"
```

构建脚本会逐字节比较 `build/CadLens.app` 与 `dist/CadLens.app` 的主程序；发现 `dist` 是旧构建会立即失败。

模型和布局截图：

```bash
./dist/CadLens.app/Contents/MacOS/CadLens \
  --snapshot /tmp/model.png "/path/to/drawing.dwg"

./dist/CadLens.app/Contents/MacOS/CadLens \
  --snapshot /tmp/layout1.png --snapshot-space 布局1 \
  --snapshot-size 17000x1000 "/path/to/drawing.dwg"
```

实体级审计：

```bash
./dist/CadLens.app/Contents/MacOS/CadLens \
  --inspect-entities "/path/to/drawing.dwg" > /tmp/entities.txt
```

## 后续扩展

- [ ] OpenGL 顶点缓冲合批，在完成更多 DWG 样本的几何回归后实施。
- [ ] 可取消的后台布局构建。
- [ ] 缩放过程中的低分辨率预览缓存。
- [ ] 只有已有性能数据证明 Qt/OpenGL 管线不足时，再评估 Metal 后端。
