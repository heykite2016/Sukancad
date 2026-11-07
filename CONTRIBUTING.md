# 参与贡献

感谢参与速看CAD开发。提交 Issue 或 Pull Request 前，请先搜索已有问题并确认使用的是当前版本。

## 报告问题

建议提供：

- 操作系统与版本。
- 速看CAD版本。
- 文件类型：DWG、DXF 或 PDF。
- 可复现步骤、预期结果和实际结果。
- 不含隐私信息的截图、日志或最小样例。

禁止上传包含客户信息、项目地址、签名、身份证件或商业机密的原始图纸。可裁切、脱敏或构造最小样例。

## 开发环境

构建步骤见 [BUILDING.md](BUILDING.md)。

提交前运行：

```bash
git diff --check
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

涉及 DWG/DXF/PDF 读取的修改，应同时验证：

- 模型空间与至少一个布局。
- 图层、颜色、线宽、填充、文字和尺寸块。
- 缓存命中与文件修改后的缓存失效。
- 无修改图纸字节。

## Pull Request

- 每个 PR 聚焦一个明确问题。
- 说明原因、实现、验证方式和已知限制。
- 新增行为应有测试或可复现的验证步骤。
- 不在仓库中提交 `build*/`、`dist/`、调试符号或大体积样例。
- 不提交访问令牌、私钥、证书密码或含隐私的图纸。
- 代码注释说明原因，不用注释重复显而易见的语句。

## 代码风格

- C++ 使用 C++20。
- 遵循现有命名、缩进和文件组织。
- 优先保持 UI 线程轻量，耗时解析和栅格化放到后台。
- 修改缓存、布局或测量语义时，同步更新 `DATA_STORAGE.md` 或相关文档。
- 不要在性能热路径中引入逐图元临时分配、重复线性扫描或无界缓存增长。

## 提交信息

推荐形式：

```text
Fix: PDF second-page rendering
Optimize: reduce hatch generation cost
Docs: add release checklist
```

## 许可

提交代码即表示你有权提供该贡献，并同意按项目的 GPL-3.0-or-later 许可分发。新增第三方代码、字体、图标或测试资源必须同时提供来源和兼容许可。

## 安全

漏洞、凭据泄露和供应链问题不要公开提交完整细节，请按 [SECURITY.md](SECURITY.md) 报告。
