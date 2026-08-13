# RenderMesh

RenderMesh 是面向公司内网 Windows 10/11 工作站的轻量级 Blender 帧级渲染农场。最终程序是约 1.2 MB 的原生 C++ 单文件 `RenderMesh.exe`：不需要 Python、.NET、安装器或管理员权限，解压后双击即可运行。

## 已实现

- 每台电脑同时是任务提交端和 Worker，通过 UDP 广播自动发现同一局域网节点。
- 共享算力默认关闭；用户主动开启后才接收别人的任务。
- 主路径针对 Eevee 和 Cycles GPU：只在电脑连续空闲（默认 3 分钟）后接单；若键鼠恢复，会中止该次渲染并安全重派。
- 极少数 Cycles CPU 任务才使用 Windows Job Object 进程树硬上限：活跃时默认 15%，空闲后 100%，可在界面修改。
- 动画按帧拆分，使用流式 HTTP 发送已打包的 `.blend`，Worker 渲染后将单帧结果回传 Origin。
- 界面显示在线电脑、信任状态、共享/活跃/空闲状态、CPU 限额、已分配帧数和任务进度。
- 配置和队列持久化到 `%LOCALAPPDATA%\RenderMesh`，写入使用临时文件原子替换。

## 零安装使用

1. 把发布 ZIP 发给办公室电脑，解压后双击 `RenderMesh.exe`。
2. Windows 第一次询问网络权限时，只允许“专用网络/域网络”。
3. 程序会自动查找已安装的 Blender；找不到时，状态栏显示“未检测到”，也可手动选择 `blender.exe`。
4. 第一台电脑复制集群邀请码，其他电脑粘贴同一邀请码并保存。
5. 愿意提供算力的电脑勾选“允许其他电脑使用本机算力”。
6. 选择 `.blend`、帧范围和输出格式，点击“预检并分发”。

程序关闭后该电脑就离线；不安装后台服务，不修改启动项。

## 提交前的硬预检

RenderMesh 不直接分发用户选择的原始工程，而是先调用本机 Blender：

1. 检查图像、字体、声音、影片、链接库、缓存和体积文件，支持 `//` 相对路径、UDIM 与序列模式。
2. 有任一外部文件丢失时阻止提交，并在界面列出路径。
3. 调用 Blender `Pack Resources`，保存独立的临时 `.blend` 副本；仍无法打包的引用会阻止提交。
4. 记录 Blender 主/次版本、渲染引擎，以及当前启用插件的模块名和可用版本号。Worker 必须使用相同主/次版本与引擎，且这些插件已安装、启用、版本一致，否则拒绝任务。
5. 在发起端把起始帧降到最多 64×64（Cycles 临时设为 1 sample）做一次真实验证渲染；相机、未定义节点、插件执行或引擎错误会在分发前阻止任务，且不改写正式 packed 工程的设置。
6. Cycles GPU 工程只会交给检测到已启用非 CPU Cycles 设备的 Worker；没有可用 GPU 的节点会拒绝接单，避免静默退回 CPU。

`.blend` 本身没有标准化、可靠的“本工程实际依赖哪些第三方插件”清单，因此当前版本采用安全优先的环境快照：记录发起端所有已启用插件，并要求 Worker 同 Blender 版本、同引擎且插件全部安装启用、已知版本一致。它能拦截清单中的插件缺失/版本错配，但无法证明一个在发起端已经缺失或禁用、且未留下可识别清单的项目插件；正式大批量任务仍建议先渲染一帧验收。

## 技术与开源基础

架构参考 [OpenCue](https://github.com/AcademySoftwareFoundation/OpenCue) 的 Frame 调度和 [CueNIMBY](https://github.com/AcademySoftwareFoundation/OpenCue/tree/master/cuenimby) 工作站自愿接单模型。原生网络和 JSON 分别使用活跃维护的 [cpp-httplib](https://github.com/yhirose/cpp-httplib) 与 [nlohmann/json](https://github.com/nlohmann/json)，两者都以 MIT 许可证使用。详情见 `THIRD_PARTY_NOTICES.md`。

## 构建与自检

开发构建使用便携版 llvm-mingw：

```powershell
powershell.exe -NoProfile -ExecutionPolicy Bypass -File .\native\build.ps1 -Configuration Release
Start-Process .\native\build\Release\RenderMesh.exe -ArgumentList '--self-test','--no-dialog' -Wait
```

内置自检覆盖配置持久化、HMAC、2 MiB 流式传输、CPU 15%→100% Job Object 策略、两个真实 HTTP 节点之间三帧任务的分发/回传、远端节点接单后掉线时自动重派，以及过期租约回传拒绝（渲染器使用测试桩）。真实 Blender 预检与渲染验证步骤见 [验证记录](docs/VERIFICATION.md)。

## 当前边界

- 只支持动画按帧拆分，不拆分同一帧的 Cycles samples。
- 每台 Worker 同时运行一个 Blender 进程。
- 已打包工程目前会逐帧上传，尚无内容哈希缓存；大型工程会重复占用局域网带宽。Worker 回传后会删除该帧临时工程；回传失败只保留小型诊断记录，避免磁盘无限增长。
- HTTP 数据面没有 TLS，集群码也不是企业身份系统；只应部署在可信公司 LAN/VLAN，不能暴露到互联网。
- CPU 可以硬限额；Windows 没有跨 NVIDIA/AMD/Intel 的通用 GPU 百分比硬配额，因此 GPU/未知渲染器采用更保守的“仅空闲运行”策略。
- EXE 当前未做商业代码签名，Windows SmartScreen 可能显示未知发布者；发布到全公司前建议由 IT 使用公司证书签名。

更多实现边界见 [架构](docs/ARCHITECTURE.md) 与 [安全说明](docs/SECURITY.md)。
