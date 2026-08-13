# RenderMesh

RenderMesh 是面向公司内网 Windows 10/11 工作站的轻量级 Blender 帧级渲染农场。最终程序是约 1.2 MB 的原生 C++ 单文件 `RenderMesh.exe`：不需要 Python、.NET、安装器或管理员权限，解压后双击即可运行。

## 已实现

- 每台电脑同时是任务提交端和 Worker，通过每张网卡的定向 UDP 广播自动发现同一局域网节点；多网卡或广播受限时可手动填写局域网 IP。
- 共享算力默认关闭；用户主动开启后才接收别人的任务。
- 主路径针对 Eevee 和 Cycles GPU：只在电脑连续空闲（默认 3 分钟）后接单；若键鼠恢复，会中止该次渲染并安全重派。
- 极少数 Cycles CPU 任务才使用 Windows Job Object 进程树硬上限：活跃时默认 15%，空闲后 100%，可在界面修改。
- 动画按实际帧序列每 5 帧组成一个任务包（末包可少于 5 帧）；已打包的 `.blend` 先按 SHA-256 查询 Worker 缓存，仅首次缺失时流式上传。每包只启动一次 Blender，包内每帧仍独立回传和记录状态。
- Eevee/Cycles GPU Worker 渲染期间检测到键鼠恢复时，不杀掉正在渲染的当前帧；当前帧完成并回传后，停止包内后续帧并交回队列。
- 界面显示在线电脑、CPU/GPU 型号、信任状态、共享/活跃/空闲状态、CPU 限额、已分配帧数和任务进度。
- 六位数字邀请码直接显示；内部仍使用随机长凭证完成发现、任务和结果鉴权。
- 配置和队列持久化到 `%LOCALAPPDATA%\RenderMesh`，写入使用临时文件原子替换；后台日志按日期写入 `logs` 目录。

## 零安装使用

1. 把发布 ZIP 发给办公室电脑，解压后双击 `RenderMesh.exe`。
2. Windows 第一次询问网络权限时，只允许“专用网络/域网络”。
3. 程序会自动查找已安装的 Blender；找不到时，状态栏显示“未检测到”，也可手动选择 `blender.exe`。
4. 第一台电脑复制六位邀请码，其他电脑粘贴并保存；第一次加入时，在“手动连接”中填写第一台电脑的局域网 `IP:端口` 完成配对。
5. 愿意提供算力的电脑勾选“允许其他电脑使用本机算力”。
6. 如果服务器列表没有自动出现另一台电脑，在对方服务器列表的“本机”行查看真实局域网地址，在本机“手动连接”中填写，例如 `192.168.28.28:8391`；不要填写 `127.0.0.1`，它始终表示当前电脑。
7. 选择 `.blend` 和帧范围，点击“预检并分发”；输出格式完全使用工程设置。

程序关闭后该电脑就离线；不安装后台服务，不修改启动项。

## 提交前的硬预检

RenderMesh 不直接分发用户选择的原始工程，而是先调用本机 Blender：

1. 检查图像、字体、声音、影片、链接库、缓存和体积文件，支持 `//` 相对路径、UDIM 与序列模式。
2. 有任一外部文件丢失时阻止提交，并在界面列出路径。
3. 调用 Blender `Pack Resources`，保存独立的临时 `.blend` 副本；仍无法打包的引用会阻止提交。
4. 记录 Blender 主/次版本、渲染引擎，以及当前启用插件的模块名和可用版本号。Worker 必须使用相同主/次版本与引擎，且这些插件已安装、启用、版本一致，否则拒绝任务。
5. 在发起端把提交范围内每一帧降到最多 64×64（Cycles 临时设为 1 sample）做真实验证渲染，并扫描材质、世界、灯光、合成器和节点组；相机、未定义节点、插件执行或引擎错误会在分发前阻止任务，且不改写正式 packed 工程的设置。帧数很多时预检时间会相应增加。
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

内置自检覆盖五帧分包、配置持久化、HMAC、文件 SHA-256、七天缓存清理、2 MiB 流式传输、CPU 15%→100% Job Object 策略、两个真实 HTTP 节点之间三帧单包经内容缓存分发/逐帧回传、远端节点接单后掉线时自动重派，以及过期租约回传拒绝（渲染器使用测试桩）。真实 Blender 预检与渲染验证步骤见 [验证记录](docs/VERIFICATION.md)。

## 当前边界

- 只支持动画按帧拆分，不拆分同一帧的 Cycles samples。
- 每台 Worker 同时运行一个 Blender 进程；每个任务包默认最多连续渲染 5 帧。
- 已打包工程使用完整 SHA-256 作为 Worker 缓存键；同一工程只在缓存缺失或 Blender/引擎/GPU/插件兼容条件变化时重新上传。最后一次任务执行超过七天后自动清理工程缓存；每帧输出临时目录在回传后立即删除，回传失败只保留小型诊断记录。
- HTTP 数据面没有 TLS，集群码也不是企业身份系统；只应部署在可信公司 LAN/VLAN，不能暴露到互联网。
- CPU 可以硬限额；Windows 没有跨 NVIDIA/AMD/Intel 的通用 GPU 百分比硬配额，因此 GPU/未知渲染器采用更保守的“仅空闲运行”策略。
- EXE 当前未做商业代码签名，Windows SmartScreen 可能显示未知发布者；发布到全公司前建议由 IT 使用公司证书签名。

更多实现边界见 [架构](docs/ARCHITECTURE.md) 与 [安全说明](docs/SECURITY.md)。
