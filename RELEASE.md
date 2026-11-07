# 发布流程

本文面向仓库维护者，描述速看CAD版本发布、产物审计和开源社区分发流程。

## 1. 更新版本

至少同步修改以下位置：

- `CMakeLists.txt` 中的 `project(CadLens VERSION ...)`
- `script/package_macos.sh`
- `script/package_windows.sh`
- `script/windows/CadLens.nsi`
- `resources/windows/CadLens.manifest`
- `resources/package/README.txt`
- `README.md`
- `CHANGELOG.md`

检查旧版本号：

```bash
rg -n --glob '!third_party/**' --glob '!build*/**' --glob '!dist/**' \
  '1\.[0-9]+\.[0-9]+|2\.0\.0' CMakeLists.txt README.md CHANGELOG.md \
  resources script src tests
```

LibreDWG 自身日志中的 `PACKAGE_VERSION` 是第三方库版本，不是应用版本。

## 2. 开发验证

```bash
git diff --check
cmake --build build -j"$(sysctl -n hw.ncpu)"
ctest --test-dir build --output-on-failure
```

发布前额外验证：

- DWG、DXF、PDF 各打开一份代表性文件。
- PDF 至少验证第二页和高分辨率打印。
- 模型空间与布局切换。
- 测量、删除、撤销和自动恢复。
- DWG/DXF 缓存失效后重新读取。

## 3. 构建全部产物

```bash
./script/build_release_all.sh all
```

生成：

```text
dist/CadLens.app
dist/CadLens-<version>-macos12-universal.dmg
dist/CadLens-<version>-macos12-universal.pkg
dist/CadLens-<version>-windows10-x64-portable.zip
dist/Sukancad-<version>-SHA256SUMS.txt
```

## 4. 审计 macOS

```bash
lipo -info dist/CadLens.app/Contents/MacOS/CadLens
lipo -info dist/CadLens.app/Contents/MacOS/libpdfium.dylib
/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' \
  dist/CadLens.app/Contents/Info.plist
codesign --verify --deep --strict dist/CadLens.app
hdiutil verify dist/CadLens-<version>-macos12-universal.dmg
pkgutil --check-signature dist/CadLens-<version>-macos12-universal.pkg
```

期望 App 和 PDFium 同时包含 `x86_64` 与 `arm64`，版本与发布版本一致。当前 ad-hoc 构建的 PKG 可能报告 `no signature`，发布说明必须如实标注，不能声称已完成 Apple 公证。

## 5. 审计 Windows

解压 portable ZIP 后检查：

```bash
llvm-readobj --file-headers CadLens/CadLens.exe
llvm-objdump -p CadLens/CadLens.exe
```

必须满足：

- `COFF-x86-64`
- PE Subsystem major version 为 `6`
- ZIP 包含 Qt、PDFium 和 MinGW 运行库
- 没有独立 `libredwg*.dll`、`libdxfrw*.dll` 或 `libiconv*.dll`

## 6. 校验和

```bash
cd dist
shasum -a 256 -c Sukancad-<version>-SHA256SUMS.txt
```

校验文件必须上传到 GitHub 与 Gitee Release，并与二进制文件同页展示。

## 7. Git 标签

确认工作树内容、测试和产物后创建带注释标签：

```bash
git tag -a v2.0.1 -m "Sukancad 2.0.1"
git show --stat v2.0.1
```

如使用签名标签：

```bash
git tag -s v2.0.1 -m "Sukancad 2.0.1"
```

不要在未确认提交内容前创建或推送标签。

## 8. 发布 GitHub Release

通过本机 `gh auth login` 或系统凭据管理器完成认证。不要在脚本、仓库文件、命令历史或 Issue 中保存访问令牌。

上传：

- DMG
- PKG
- Windows portable ZIP
- SHA-256 清单
- GitHub 自动生成的源码包

Release 正文可直接采用 `CHANGELOG.md` 中对应版本段落，并补充签名和公证状态。

## 9. 发布 Gitee Release

使用 Gitee 网页或本机凭据管理器上传相同文件和 SHA-256 清单。核对：

- GitHub 与 Gitee 的标签和版本一致。
- 两个平台的二进制文件来自同一次发布构建。
- README 下载链接指向已存在的 Release。
- Release 说明包含已知限制。

## 10. 发布后检查

```bash
git ls-remote --tags origin 'refs/tags/v2.0.1'
git ls-remote --tags gitee 'refs/tags/v2.0.1'
```

再用未登录浏览器检查下载链接，确认 GitHub 和 Gitee 均可访问。
