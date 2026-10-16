# 开发与构建

RenderMesh 使用 C++20 和 Windows 原生 API。当前构建目标为 Windows x64。

## 1. 准备环境

需要：

- Windows 10/11 x64。
- Git。
- Windows PowerShell 5.1 或更新版本。
- 便携版 llvm-mingw UCRT x86_64 工具链。
- 运行 Python 回归脚本时需要 Python 3；它不是程序运行依赖。
- 运行真实渲染测试时需要 Blender。

已用于此项目的工具链为 [llvm-mingw 20260616](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616)。使用 Windows 的 `ucrt-x86_64.zip` 包。源码内已包含 nlohmann/json 3.12.0，无需安装包管理器。

```powershell
git clone https://github.com/FELIXDAD/RenderMesh.git
cd RenderMesh
New-Item -ItemType Directory -Force tools | Out-Null
Invoke-WebRequest -Uri "https://github.com/mstorsjo/llvm-mingw/releases/download/20260616/llvm-mingw-20260616-ucrt-x86_64.zip" -OutFile tools\llvm-mingw.zip
Expand-Archive -LiteralPath tools\llvm-mingw.zip -DestinationPath tools
```

确认以下文件存在：

```text
tools/llvm-mingw-20260616-ucrt-x86_64/bin/clang++.exe
tools/llvm-mingw-20260616-ucrt-x86_64/bin/llvm-windres.exe
```

构建脚本从 `tools/llvm-mingw-*-ucrt-x86_64` 中按名称倒序选择第一个目录。保留一个所需工具链即可。

## 2. 构建程序

在仓库根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1 -Configuration Release
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1 -Configuration Release -DevTools
```

| 文件 | 用途 |
| --- | --- |
| `native/build/Release/RenderMesh.exe` | 分发程序，含桌面、节点与用户 CLI |
| `native/build/Release/RenderMesh-dev.exe` | 开发程序，另含内置自检和 UI 测试入口 |

使用 `-Configuration Debug` 可生成调试构建。使用 `-OutputDirectory` 可更改输出目录；目录必须位于仓库内。

## 3. 内置自检

```powershell
$exe = (Resolve-Path .\native\build\Release\RenderMesh-dev.exe).Path
$report = Join-Path (Split-Path $exe) "self-test.txt"
$process = Start-Process -FilePath $exe -ArgumentList "--self-test", "--no-dialog", "--report", ('"{0}"' -f $report) -WindowStyle Hidden -Wait -PassThru
Get-Content -LiteralPath $report
if ($process.ExitCode -ne 0) { throw "Self-test failed: $($process.ExitCode)" }
```

自检覆盖配置、任务状态、包调度、缓存、HTTP 传输、结果和日志回传等逻辑。渲染部分使用测试桩；真实 Blender 测试见下一节。

UI 回归需要开发版：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\verify-ui.ps1 -SkipBuild -Executable .\native\build\Release\RenderMesh-dev.exe
```

## 4. 真实 Blender 与 CLI 测试

下面测试会在临时目录创建小型工程、启动独立节点，并执行工程检查、素材打包、测试帧和完整渲染。第二个参数改为本机实际路径。

```powershell
python .\native\test-cli-import.py .\native\build\Release\RenderMesh.exe "C:\Program Files\Blender Foundation\Blender 5.2\blender.exe"
```

成功输出 `CLI_IMPORT_OK`。测试使用单独的数据目录和可用端口，不需要手动准备工程。

更多专项脚本位于 `native/test-*.ps1` 和 `native/test-*.py`。真实跨电脑渲染还需要在目标 LAN 和硬件上验证；本机回环测试不代替这一步。

## 5. 打包

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\package.ps1
```

打包脚本会：

1. 构建分发版和开发版。
2. 运行内置自检、UI 回归及传输、发现、更新等专项检查。
3. 有 Python 时执行历史任务导入测试。
4. 生成分发 EXE 和 ZIP。

ZIP 包含 `RenderMesh.exe`、中文快速指南、项目许可证和第三方声明。Blender、工具链和开发 EXE 不随包分发。

已有对应构建时可用 `-SkipBuild`。已有同版本 EXE 且内容不同时，脚本拒绝覆盖；发布不同二进制前先更新版本。版本涉及：

- `native/src/Core.hpp`
- `native/RenderMesh.rc`
- `native/package.ps1` 的默认包名
- README、使用指南和快速指南中的版本

当前默认产物为 `dist/RenderMesh-0.3.0.exe` 和 `dist/RenderMesh-Windows-x64-0.3.0.zip`。发布到 GitHub Releases，不放入源码提交。

## 6. 代码目录

| 路径 | 职责 |
| --- | --- |
| `native/src/Core.*` | 配置、数据类型、文件、哈希和公共逻辑 |
| `native/src/Http.*` | HTTP/1.1 客户端与服务端 |
| `native/src/Node.cpp` | 节点发现、任务调度、传输、缓存、持久化和更新 |
| `native/src/Render.hpp` | 渲染软件适配器接口 |
| `native/src/BlenderAdapter.cpp` | Blender 检查、打包、渲染和预览 |
| `native/src/BlenderInstaller.*` | 官方 Blender 版本查询和 MSI 下载 |
| `native/src/Cli.*` | 用户命令行和本机控制请求 |
| `native/src/UiModel.*` | 界面状态投影 |
| `native/src/WinMain.cpp` | 原生窗口、交互、托盘和程序入口 |
| `native/src/LegacyJobs.cpp` | 读取历史任务记录 |
| `native/third_party/` | 随源码分发的第三方依赖 |

实现细节见 [架构](ARCHITECTURE.md)。

## 7. 提交改动

先确认要修复的行为和复现条件。保持改动聚焦，更新受影响的说明，并运行与改动有关的检查。PR 说明应包含最终行为、复现或使用步骤和验证结果。

只提交源代码、测试、资源和文档。不要提交编译器、EXE、ZIP、运行时配置、日志、工程、渲染结果或凭证。示例工程优先由测试脚本生成。

项目使用 Apache-2.0；引入依赖时保留上游许可，并更新 [第三方声明](../THIRD_PARTY_NOTICES.md)。
