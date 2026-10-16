<p align="center">
  <img src="native/assets/RenderMesh-icon.png" width="112" alt="RenderMesh">
</p>

# RenderMesh

**把局域网内空闲的 Windows 电脑用于 Blender 动画渲染。**

RenderMesh 把动画分成多个帧任务，交给其他电脑渲染，再把图像收回提交电脑。每台电脑都可以提交任务，也可以提供算力。无需部署中心服务器。

原生 C++ 桌面程序，当前版本 **0.3.0**。Windows x64 EXE 约 1.6 MB，解压后即可运行。RenderMesh 本身不需要 Python 或 .NET；渲染需要单独安装 Blender。

[下载 Windows 版](https://github.com/FELIXDAD/RenderMesh/releases/latest) · [使用指南](docs/USER_GUIDE.md) · [命令行](docs/CLI.md) · [从源码构建](docs/DEVELOPMENT.md) · [反馈问题](https://github.com/FELIXDAD/RenderMesh/issues)

## 可以做什么

- **连接电脑**：输入相同的六位匹配 ID，自动发现同一局域网内的节点。也支持手动连接 IPv4 地址。
- **使用空闲算力**：共享默认关闭。开启后，电脑连续空闲达到设定时间才接收远端任务，默认 3 分钟。用户恢复操作时，完成当前帧后停止后续帧。
- **按帧分配任务**：每包 1–20 帧，默认 5 帧。每台工作电脑同时运行一个 Blender 渲染进程。
- **传输工程**：检查外部素材，按需打包。工程旁的 `tex` 文件夹随工程传输。大文件支持分块断点续传和缓存复用。
- **检查结果**：先渲染测试帧，再启动完整任务。逐帧查看状态、时长、错误、日志和输出。
- **保留输出**：收回主图和合成器 File Output 节点产生的文件，并写入提交电脑上对应的输出目录。
- **管理任务**：停止、恢复、重试、删除任务，并调整参与渲染的电脑。
- **接入脚本**：提供命令行和 JSON 输出，可用于自动化操作。

## 使用条件

| 项目 | 要求 |
| --- | --- |
| 系统 | Windows 10/11，x64 |
| 渲染软件 | Blender；提交端与工作电脑使用相同主、次版本 |
| 渲染引擎 | Eevee、Cycles CPU、Cycles GPU |
| GPU 任务 | 工作电脑须通过对应 GPU 检查；Cycles GPU 任务不会自动改用 CPU |
| 网络 | 可信局域网，默认 UDP 8390、TCP 8391 |
| 输出 | 使用逐帧图像序列，例如 PNG 或 OpenEXR |
| 磁盘 | 能容纳工程副本、传输缓存和渲染结果 |

当前适配器只支持 Blender。RenderMesh 按帧分工，不把同一帧拆给多台电脑。第三方插件、模拟缓存和不可打包的外部文件需要自行准备一致的环境。

## 第一次渲染

1. 从 [Releases](https://github.com/FELIXDAD/RenderMesh/releases/latest) 下载 ZIP，在每台电脑解压并运行 `RenderMesh.exe`。
2. 安装 Blender。在设置页确认其可执行文件路径；自定义安装位置可用“手动指定…”选择。
3. 给各台电脑设置相同的六位匹配 ID。确认它们出现在服务器列表中。
4. 在提供算力的电脑上开启“共享本机算力”，等待电脑达到空闲时间。
5. 在提交电脑的“渲染任务”页添加 `.blend`。检查素材警告，等待工程同步。
6. 选择渲染配置、帧范围、每包帧数和测试帧。先完成测试帧。
7. 检查测试输出。任务显示“测试完成”后，右键选择“全速渲染”。

提交电脑负责调度和收集结果，渲染期间保持运行。没有可用的远端工作电脑时，任务可以使用本机渲染。完整步骤、输出位置和排错方法见 [使用指南](docs/USER_GUIDE.md)。

## 命令行示例

先打开桌面程序。在 PowerShell 中运行：

```powershell
.\RenderMesh.exe version | Out-Host
.\RenderMesh.exe status --json | Out-Host
.\RenderMesh.exe inspect "D:\Projects\shot.blend" --json | Out-Host
.\RenderMesh.exe submit "D:\Projects\shot.blend" --name shot010 --wait --json | Out-Host
```

`submit` 创建并同步任务。之后用 `test` 渲染测试帧，测试完成后再用 `start` 渲染完整范围。参见 [命令行指南](docs/CLI.md)。

## 数据和网络

配置、日志和任务记录默认保存在 `%LOCALAPPDATA%\RenderMesh`。工程副本及缓存的位置可以在设置页的“工程存储”中更改。原始工程与最终输出使用各自的文件路径。

RenderMesh 使用明文 HTTP 传输数据。六位匹配 ID 用于局域网配对，不是企业身份认证。只在可信 LAN/VLAN 内使用，不要把服务端口映射到互联网。运行时没有安装 Windows 服务；开机自启由用户在设置页选择。详见 [安全说明](docs/SECURITY.md)。

## 构建

使用 Windows、PowerShell 和便携版 [llvm-mingw](https://github.com/mstorsjo/llvm-mingw/releases/tag/20260616)。把工具链解压到 `tools/llvm-mingw-20260616-ucrt-x86_64/`，从仓库根目录运行：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1 -Configuration Release
```

产物为 `native/build/Release/RenderMesh.exe`。[开发指南](docs/DEVELOPMENT.md) 包含工具链安装、自检、真实 Blender 测试及打包步骤。

## 项目文档

| 文档 | 内容 |
| --- | --- |
| [使用指南](docs/USER_GUIDE.md) | 安装、组网、提交任务、输出、设置和排错 |
| [命令行](docs/CLI.md) | 命令、参数、JSON、状态与退出码 |
| [开发指南](docs/DEVELOPMENT.md) | 构建、测试、打包和贡献说明 |
| [架构](docs/ARCHITECTURE.md) | 调度、传输、缓存、持久化和适配器 |
| [安全说明](docs/SECURITY.md) | 配对、权限、数据传输和信任边界 |
| [验证记录](docs/VERIFICATION.md) | 各版本的验证范围及历史结果 |

## 许可证与致谢

项目采用 [Apache-2.0](LICENSE) 许可证，Copyright 2026 RenderMesh contributors。第三方组件及其许可证见 [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md)。

使用 [nlohmann/json](https://github.com/nlohmann/json) 处理 JSON，部分托盘状态图标源自 Microsoft Fluent UI System Icons。调度设计参考 OpenCue 的帧任务模型和工作站自愿接单方式。Blender 需单独安装，未包含在下载包中。
