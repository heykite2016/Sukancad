# 数据、缓存与只读策略

## 原始图纸

DWG、DXF 和 PDF 始终以只读方式打开。速看CAD 不会写回原始图纸。

## 正式批注

正式保存的批注使用 JSON v2，文件位于用户应用数据目录：

- macOS：`~/Library/Application Support/CAD Lens/annotations/<稳定路径哈希>.cadlens.json`
- Windows：`%APPDATA%/CAD Lens/annotations/<稳定路径哈希>.cadlens.json`

文件包含：

- 原图纸稳定路径和文件签名。
- 单位、比例、标注样式、标注线宽和“标注随画布缩放”选项。
- 规范化线段/圆弧几何。
- 批注颜色、可见状态、备注和批注层。

正式批注会长期保留，不随“最近图纸”列表移除而删除。

## 自动恢复副本

自动恢复副本位于系统缓存目录：

- macOS：`~/Library/Caches/CAD Lens/annotations-autosave`
- Windows：`%LOCALAPPDATA%/CAD Lens/Cache/annotations-autosave`

自动保存采用 400 ms 防抖和原子写入。显式保存成功后删除对应自动副本；未保存副本保留 30 天。

## 其他缓存

- 最近图纸列表默认保留 12 条。
- 最近图纸缩略图随最近列表清理。
- DWG/DXF 解析缓存对最近图纸复用；非最近图纸的解析缓存保留 30 天。
- 画布质量和 CAD 线宽显隐写入本机应用设置，不修改原始图纸。
- 每张已打开图纸的屏幕几何缓存限制为最多 32M 像素；多页打印预览总量限制为 48M 像素。
- 删除缓存、缩略图或最近记录不会删除正式批注。
