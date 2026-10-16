# 验证记录

验证日期：2026-08-14 起，最近一次为 2026-10-16（0.3.0），Windows x64。

## 0.3.0 公开发布验证（2026-10-16）

- 使用 llvm-mingw 20260616 从当前源码构建 Release 成功。产物为 1,608,704 字节，与分发 EXE 仅有 PE 编译时间戳差异。
- `native/package.ps1 -SkipBuild` 全部通过：内置自检、UI 回归、大工程传输策略、局域网更新包、更新应用、单实例、自动配对、节点交换、直接状态刷新、无中心发现、空闲状态及历史任务导入。
- `native/test-cli-import.py` 使用真实 Blender 5.2 执行，输出 `CLI_IMPORT_OK`。覆盖普通工程、外部素材、`tex` 工程包、缺失素材检查、CLI 任务操作，以及测试帧和正式帧的实际输出。
- 发布 ZIP 含分发 EXE、当前中文快速指南、完整 Apache-2.0 许可和第三方声明。

网络回归在本机独立节点进程间执行。本次真实 Blender 测试使用临时生成的小工程；部署到其他工作电脑时仍需按使用指南检查目标 Blender、插件、GPU 与素材环境。

以下保留各版本的历史验证记录。

## 原生程序自检

`RenderMesh-dev.exe --self-test --no-dialog` 返回 0（0.3.0 起自检只编入 `-DevTools` 构建，分发的 `RenderMesh.exe` 不含自检代码），覆盖：

- 配置原子保存与重新读取、旧插件缓存字段不参与兼容判断，以及标准向量 HMAC-SHA256；
- `/23` 等 IPv4 前缀的定向广播地址计算，且每张网卡保留独立源地址；
- 2 MiB 文件通过真实本机 HTTP 服务流式上传；
- 仅空闲接单策略的阈值边界；
- 手动 `IPv4:端口` 先经过 `/health` 与 HMAC 挑战验证并持久化，再由两个独立 NodeApp 实例通过不同 TCP 端口将七帧分为 5+2 任务包，经工程缓存回传主输出与合成器 EXR；
- 停止远端五帧包时，取消信号与派发竞态均被栅栏拦截；若 Worker 已开始，只允许当前帧收尾，后续帧不再启动；
- Worker 任务包贡献历史会记录分配的五帧、已完成的五帧与开始/结束时间，重建 NodeApp 后仍可读取；
- `RMBNDL1` 多文件包的安全路径、`../` 穿越拒绝、取消时临时文件清理和输出盘发布回滚；
- 错误类型的节点 JSON 会被拒绝而不会终止后台线程；旧版 Worker 不报告任务 ID 时，`assigned=1` 的长帧不会被误判为掉线，恢复空闲后仍可触发重派；
- 模拟 Worker 接单后离线，Origin 在租约宽限期后自动把该帧恢复 Pending，并由本机完成；
- 旧 Worker 使用已经失效的 attempt token 迟到回传时，Origin 返回 409，不会覆盖重派结果。
- 模拟 Worker 确定性不兼容与暂时忙碌：七帧任务都只请求一次该节点，随后分别按任务屏蔽或 5 秒退避，并立即以本机 5+2 包完成；不存在每 250 ms 重传工程的重试风暴。

## 真实 Blender 5.2 LTS

测试工程引用 `//linked/checker.png`：

1. 素材存在：预检通过，Blender 版本为 5.2，`missing=0`、`unpacked=0`，生成独立 packed `.blend`；报告不再采集插件清单。
2. 删除原始 `checker.png` 后：RenderMesh 用 packed 副本成功生成 64×64 PNG，证明渲染不依赖原相对路径素材。
3. 对未打包原工程删除 `checker.png`：预检返回 3 并报告该 image 的绝对缺失路径，任务未创建。
4. Worker 环境与记录一致：兼容性检查返回 0。向旧格式报告加入不存在的插件及版本 `legacy_missing_addon@999.0` 后仍返回 0，证明插件安装、启用和版本审查已关闭。
5. 把 Blender 主/次版本改为不一致值时兼容性检查仍返回 5；引擎和 GPU 探针也继续参与兼容判断。
6. VSE Image Strip 使用相对素材路径：删除原图后，预检通过 `bpy.utils.blend_paths` 将它报告为 `external` 缺失项并阻止提交。
7. 无摄像机工程旁预放一个旧 `frame_000099.png`：请求第 1 帧返回失败，不会把旧帧误认为新结果。
8. 图片序列缺少中间帧 `seq_0002.png`：预检精确报告缺失帧并返回 3；序列完整但 Blender 无法完整 pack 时报告全部 unpacked 路径并阻止分发。
9. Eevee、Cycles GPU 和 Cycles CPU 的远端任务都只在电脑空闲时接单；Cycles GPU 另外要求 Worker 检测到已启用的非 CPU Cycles 设备。
   Eevee 兼容性检查成功完成 1×1 真实渲染上下文探针并报告 `NVIDIA Corporation:NVIDIA GeForce RTX 5090 D v2`；本机 Cycles GPU 探测报告 `CUDA:NVIDIA GeForce RTX 5090 D v2` 和 `OPTIX:NVIDIA GeForce RTX 5090 D v2`，两种兼容性检查均返回 0。
10. 提交前在发起端对提交范围内每一帧执行低分辨率真实渲染，并检查材质、世界、灯光、合成器和节点组；测试工程返回 `validation_render=true` 和准确的 `validation_frames`。无相机或插件/节点/引擎执行异常时预检返回 3，不创建任务。
11. 用户于 2026-08-14 更新的 `D:\05_Documents\Desktop\测试.blend` 在 Blender 5.2 预检通过：Cycles GPU、PNG、`missing=0`、`unpacked=0`，主输出映射为 `D:\05_Documents\Desktop\tmp\####.png`，并检测到映射至 `tmp\AOV` 的 File Output 节点。本机真实提交 1–7 帧后只启动两个 Blender 进程（5+2），七张 PNG 和合成器 EXR 均发布到工程目录。
12. 另建 Blender 5.2 合成器工程，一次 Blender 启动同时生成主 PNG 和 File Output OpenEXR；清单准确记录 `primary` 与 `compositor/output-0/rgba_.exr`，并保留原节点输出目录。
13. 将上述真实 OpenEXR 交给桌面端预览转换路径，Blender 5.2 成功生成可由 WIC 显示的 PNG 缩略图。
14. GUI 运行时度量验证任务 ListView 的 `LVM_GETCOUNTPERPAGE=10`，主共享开关文字在 150% DPI 的可用区域内完整显示。
15. 双机实测发现测试 Worker 无法应用 Windows CPU Job 限速时，旧策略会以内部代码 91 终止 Blender 兼容检查。0.2.2 移除 CPU 百分比限速器和界面设置，兼容检查及所有远端渲染统一只在闲置状态运行；策略边界由内置自检覆盖。Blender 仍使用不含 CPU rate control 的 `KILL_ON_JOB_CLOSE` 生命周期 Job，受管环境无法加入时不会阻止渲染。
16. 0.2.3 关闭插件清单审查：发起端不再采集或持久化插件字段，新版 Worker 忽略旧任务/缓存中的插件字段，新版 Origin 向旧 v6 Worker 发送空列表；内置回归自检验证带任意旧插件字段的缓存仍可命中。
17. 对在线测试节点 `192.168.28.28:8391` 走真实 v2 缓存/兼容链路：同一 packed 工程最初查询返回 404；携带 `X-RM-Addons: []` 上传后，远端 Blender 兼容检查返回 201 `cached`；再次查询返回 200。测试节点为 Blender 5.2/Cycles GPU 环境，证明旧严格插件元数据不再阻止该工程进入远端缓存与接单路径。
18. 0.2.3 双机最小探针复现远端返回 202 后任务包瞬时清零；结果回传为 `status=failed` 且错误为空。根因是 Blender 退出码为 0、但进度清单没有完成帧时 `RenderBlenderPackage` 仍返回成功。0.2.4 增加零帧/部分帧完成验证、10 秒内空产出自动重启一次，以及失败结果持久日志。回归自检先在旧行为上以 `Zero-output Blender package validation self-test failed` 返回 1，修复后返回 0；同一真实测试工程的主 PNG 与合成器 EXR 仍成功生成。
19. 0.2.5 在隔离且完全空白的 `BLENDER_USER_CONFIG` 中走真实 Worker 链路：首次兼容检查在约 2 秒内生成 `userpref.blend` 和原子就绪文件，Blender 5.2/Cycles 检查只启用 `OPTIX:NVIDIA GeForce RTX 5090 D v2`；第二次运行前后偏好 SHA-256 不变。随后同一空配置起点完成真实单帧渲染，日志记录所选 OptiX 设备，并生成主 PNG 与合成器 EXR。内置测试先以 `Blender first-run readiness self-test not implemented` 返回 1，完成启动参数、就绪文件和单后端约束后返回 0。
20. 0.2.6 使用 `D:\05_Documents\Desktop\测试.blend` 验证工程读取与 AOV 回传：无界面检查准确得到起止帧 `1–36`、步长 `1`，定位到节点路径 `4`，并读取 Blender 5.2 `file_output_items` 的 6 个输出项 `Output/Alpha/Mist/Emission/CryptoObject00/Noisy Image`。真实第 1 帧渲染返回 0，结果清单同时包含 4,612,054 字节主 PNG 与 6,213,778 字节多层 EXR `compositor/output-0/prepared.exr`；配置的 File Output 节点若未生成文件会明确失败，不再允许只有主图的假完整结果。
21. 0.2.7 在公开 `NodeApp::SubmitPrepared` seam 上以不允许共享的 Windows 文件句柄独占 packed 工程 250 ms：旧实现稳定返回 `Transient locked-source submission self-test failed: 无法复制已打包工程`；加入仅针对瞬时文件竞争的 50 ms/最长 5 秒重试后，同一测试创建任务成功，完整内置自检返回 0。永久错误与超时仍返回原 Windows 错误，不会创建半任务。
22. 0.2.8 在双 NodeApp 公共 seam 中把五帧 Worker 测试包设为逐帧可观察：旧实现必须等五帧结束，自检以 `per-frame result streaming self-test failed` 返回 1；改为运行中消费原子清单后，第 1 帧主图与 AOV 已在 Origin 落盘时后四帧仍未完成，完整自检返回 0。随后用 `D:\05_Documents\Desktop\测试.blend` 和 Blender 5.2 做真实第 1 帧验证，清单同时记录主 PNG 与 6,213,274 字节多层 EXR `compositor/output-0/测试_000001.exr`；原 `{blend_name}` 不再退化为缓存名 `source`，且固定文件名补帧号避免包内覆盖。

23. 0.2.9 把作业元数据迁移到静态 SQLite WAL，历史列表按提交时间倒序且不按数量截断；终态满七天后保留“（过期）”元数据并清理托管副本。内置测试覆盖 `jobs.json` 导入后删除不复活、优先级/FIFO 比较、主序列与合成器结果原子另存、协议 v8 清理能力签名、远端缓存删除和确定性的活动投影；UI 回归继续验证 1 Hz 空闲心跳、16 ms 突发合帧、滚动/选择/焦点保持及无全表重置。
24. 0.2.10 增加单帧 Blender 日志：五帧包共用的 stdout/stderr 由显式帧边界切分，首帧保留启动/工程加载信息，ANSI 控制序列被移除，单帧超过 4 MiB 时保留首尾并标记截断。双 NodeApp 自检要求第 1 帧结果到达而后续帧仍在渲染时对应日志已经落盘，并验证全部五帧最终日志；Core 自检覆盖边界切分、首帧前缀、控制序列清理和无变化不重写。随后用 Blender 5.2 对 `D:\05_Documents\Desktop\测试.blend` 真实渲染第 1 帧，生成 2,563 字节单帧日志、4,612,055 字节主 PNG 和 6,213,383 字节合成器 EXR，日志不含内部帧边界标记并记录实际 Cycles GPU。
25. 0.2.11 增加新任务向导和协议 v9：`.blend` 拖放/右下角添加会先本地预检，再以 8 MiB 分块、块 SHA-256、远程偏移查询和整体 SHA-256 提交同步到所有参与节点；连续三次网络失败进入“上传失败”，手动重试从已保存偏移继续。双 NodeApp 自检覆盖工程进入远程缓存、`1,4,7` 测试帧只在 Worker 执行、状态停在“测试完成”、释放全速渲染后其余帧全部完成。原生 UI 回归验证 100 行任务下无全表重置，单行变更只产生 1 次列表修改，选中、滚动和焦点保持。对 `D:\05_Documents\Desktop\测试.blend` 的无界面实际检查返回帧范围 `1–36`、步长 `1`、1 个 File Output 节点和 6 个输出项。
26. 0.2.12 修复新建任务入口的命中热区：响应式布局为任务列表底部预留 90 像素，使右下角加号与列表保持 16 像素间距。独立 Win32 回归探针验证加号中心实际命中 `ID_ADD_JOB`，并在标准按钮点击后出现 Blender 工程选择对话框；该探针已纳入 `verify-ui.ps1`。
27. 0.2.13 修复远端单帧日志与结果回传的竞态：Origin 可在结果先完成时继续接收同一 attempt 的最终日志，但拒绝过期或已重新分配的 attempt；日志上传使用独立暂存目录，不会被结果包清理误删。双 NodeApp 自检固定模拟“结果先到、日志后到”，并要求五帧结果和对应 Blender 日志全部落盘。
28. 参数窗口增加可复用渲染软件配置并记住上次选择：配置持久化自检覆盖两套 Blender 配置的名称、系统、软件、路径和选中 ID 往返；旧 `blender_path` 自动迁移。任务与本机贡献 ListView 移除单选限制，UI 回归要求两行选择在增量刷新后仍保留，并验证批量全速渲染、批量删除和所选贡献历史清理入口存在。贡献清理自检在进程重载后删除全部选中 package ID，再次重载仍为空。
29. 匹配 ID 信任隔离回归使用两个真实独立进程：先让 `123456` 与 `777777` 共用旧版遗留的同一内部凭证，双方 `peer_count` 必须均为 0；再从同一 `123456` 组把两端都改为 `777777`，程序通过已保存的本机地址自动配对，双方 `peer_count` 均恢复为 1、待匹配状态清除、内部凭证一致且不再等于旧凭证。UI 回归同时要求服务器页忽略手动 IP 探测详情，稳定状态只显示“已就绪”“正在邀请码配对”或“未检测到 Blender”。
30. 0.2.17 将匹配 ID 绑定正式升级为协议 v10，并在 `/health` 暴露应用版本。真实双进程回归同时覆盖三种情况：不同 ID 即使遗留凭证相同也保持 `peer_count=0`；两端改为同一 ID 后自动配对并恢复为 `1,1`；当前节点面对不含 binding、但同样自称 protocol 9 的旧 EXE 时拒绝加入调度池，并明确提示两台电脑升级到 0.2.17，不再误报“匹配 ID 不一致”。
31. 0.2.18 升级协议 v11：独立双进程从不同内部凭证、空手动地址和相同 `111111` 开始，通过 UDP 配对候选广播自动完成 `/v2/pair`，双方退出待匹配、凭证一致且 `peer_count=1,1`。Blender 可用状态加入发现签名和手动连接 challenge 证明；已信任、已共享但 `blender_ready=false` 的节点不会进入工程同步或调度池。
32. 0.2.19 增加协议 v12 认证节点交换：三进程分别使用互不相通的 UDP 发现端口，Hub 与既有 Peer 先互信，新节点只保存 Hub 地址并从独立凭证开始；旧实现稳定停在 `peer_count=1`，新实现从 Hub 获取带 HMAC challenge 证明的在线目录后再逐台执行完整 `/health` 与 `/v1/prove`，最终新节点为 `peer_count=2`。混合测试把既有 Peer 固定为 0.2.18/protocol 11 仍通过，避免要求全网一次性升级。
33. 0.2.20 增加协议 v13 直接状态快照：三进程使用隔离 UDP 端口，Observer 先经 Hub 获知 Worker；Worker 从 `share=true` 重启为 `share=false` 后，旧实现只从 Hub 目录刷新 `lastSeen` 并永久保留旧状态，新实现直接挑战 Worker 的 `/health`，Observer 与 Hub 均读到 `share=false`。该回归同时覆盖第三方旧 `task_id` 不再触发 Assigned/Rendering 帧回退 Pending。
34. 三个全新节点均不填写手动 IP、使用相同六位匹配 ID 和同一发现端口启动，全部自动收敛到同一内部凭证且各自 `peer_count=2`。随后完全退出第一个“测试机”、清空剩余节点的手动地址并只重启后两台，两台仍通过定向广播/局域网组播互相发现，各自 `peer_count=1`，证明不存在隐藏协调器。
35. 0.2.21 增加协议 v14 签名空闲资格：真实 `192.168.28.28:8391/health` 连续采样为 protocol 13、idle `153→155`，证明 Windows 计时和直接状态更新正常；旧界面回归稳定把 `idle=0` 错写成“已空闲 0 秒”。新回归要求 `idle_ready=false` 显示黄色“活跃 · 等待”，并用双进程隔离端口验证 Observer 收到 Worker 的签名 `idle_ready=false`；调度器也不会选择尚未达到自身空闲阈值的 v14 Worker。
36. 0.2.22 详情动作与本机状态回归：旧界面在 `TestCompleted` 没有详情主动作，且本机 `assigned=1` 仍按 idle 状态显示。新 UI 模型要求测试完成映射为 `FullRender`、Testing/Rendering 映射为 `Stop`，两种动作互斥且共用资源管理器按钮右侧的最右 48×48 槽；绿色播放按钮调用既有 `StartFullRender`，阶段切换后立即被红色停止按钮替换。本机状态回归固定要求 `LocalComputeStatusText(..., assigned=1)` 返回“正在渲染”，视觉状态为 Active。
37. 0.2.23 增加停止任务恢复和真实右键菜单回归：`ResumeStoppedJob` 只接受 `Stopped` 作业，保留 Completed 帧及其输出，只把 Stopped 帧清除旧分配/租约/临时结果后改回 Pending；测试帧未完成时恢复到 Testing，全部测试帧已完成或无测试门时恢复到 Rendering，并用新的取消信号隔离停止前仍在退出的旧执行。菜单回归通过真实 ListView `NM_RCLICK` 打开 TestCompleted 作业的“全速渲染”，旧实现在命令执行后稳定残留一个 `#32768` 菜单窗口；新实现等待右键通知周期排空后再打开菜单，连续执行后均无残留。
38. 0.2.24 增加素材警告的显式安全例外：`test-missing-asset-override.ps1` 用 Blender 创建一个保留但未连接到场景的缺失图像引用；第一次预检必须退出 3、保留缺失清单并报告 `can_ignore_asset_warnings=true`，选择 `--ignore-asset-warnings` 后必须保存新的 prepared `.blend`、逐帧完成真实 Eevee 验证、退出 0，同时保留 `asset_warnings_ignored=true` 与缺失清单。内置真值表固定“已放行、元数据无效、无素材警告或验证报错”均不得再次显示例外入口；UI 回归要求使用 RenderMesh 内部双按钮警告面，安全默认焦点是“取消”。
39. 0.2.25 按用户确认把“无视警告”改为全量覆盖：任何已成功读取基本工程元数据的预检失败都提供该按钮，不再要求必须是素材错误；点击后跳过素材/插件/节点/相机/引擎警告和低清真实预检渲染，直接生成待上传任务。真实 Blender 固件要求第二次报告 `validation_skipped_by_user=true`、`asset_warnings_ignored=true`、`pass=true`，并继续保留缺失路径供日志审计；UI 回归固定按钮文案、默认取消、Esc 关闭和“直接进入上传/参数设置”说明。
40. 0.2.26 增加新建任务三阶段阻塞进度和单帧真实计时回归：UI 测试要求进度层覆盖完整客户区、位于添加按钮命中热区上方、取得键盘焦点且显示非零阶段进度；失败/完成和窗口关闭路径均清理遮罩。双节点五帧测试桩每帧固定渲染 150 ms，旧实现五帧在包分配时几乎同时写入 `started_at`，新增断言稳定失败；新实现把每帧在 Blender/Worker 内部的独立开始与结束时间随该帧结果回传，要求相邻帧开始时间至少相隔 100 ms，整套自检恢复为 exit 0。
41. 0.2.27 把“任务包已接收”和“Blender 已开始当前帧”拆成两个状态：双节点五帧回归要求首帧完成时，后续至少一帧仍为 `Assigned/排队中`、`started_at` 为零，且同一包最多一帧为 `Rendering/渲染中`。Worker 从 Blender 原子进度清单读取单帧开始时间，经带 token/task/attempt 的 `/v1/frame-start` 通知 Origin；结果、日志、掉线恢复和停止路径均接受新的有效租约状态。UI 回归同时要求本机及远端在已接包但未开始帧时显示“排队中”，开始后显示“正在渲染”。
42. 0.2.28 增加协议 v15 `frame_start` 能力协商：真实网络诊断确认两台仍运行 0.2.25/protocol 14 的 Worker 不包含该字段，解释了新版 Origin 上远端帧只显示“排队中”并在结果到达时直接完成。回归要求该能力进入发现 HMAC、`/health` 与认证元数据；未声明能力的节点强制单帧包，当前节点仍保持多帧。双节点实时回归收紧为必须同时观察到恰好一帧 `Rendering` 和同包后续 `Assigned`。停止回归必须先观察 Worker 已进入模拟打开 Blender 的 500 ms 加载窗口、仍未开始首帧，再发出 `/v3/cancel`；测试要求产生零完成帧的 stopped 贡献记录，Origin 保持 Stopped，Worker 的 `assigned` 和活动状态均在三秒内归零。一旦包内任何帧真正开始，独立阶段锁存会一直阻止硬杀，避免结果上传阻塞进度轮询时误杀已开始的下一帧。
43. 0.2.29 增加动态渲染主机、通知区域、局域网更新和同级 `tex` 工程包。内置双节点自检在任务已创建后加入并移除参与者，要求新增节点先同步工程、被移除节点不再派发且选择持久化；停止回归继续固定 Blender 打开工程窗口并要求 Worker 占用清零。工程包回归创建 `tex/albedo.png` 与 `tex/sequence/0001.exr`，先做格式往返，再经真实 HTTP 断点上传、缓存提交与远端测试帧执行，要求 Worker 缓存中的层级和内容一致；空 `tex` 目录也必须在 Worker 恢复。安全回归手工构造含 `tex/.. /.. /escaped.txt` 的恶意包，要求 Windows 归一化路径、尾随空格/句点、设备名和 ADS 全部在写文件前被拒绝且目录外哨兵文件不变。更新回归由一个无 Blender 的受信节点提供更高版本清单与测试 EXE，客户端必须完成新鲜端点证明、HMAC 清单验证、长度/SHA-256 下载验证，并把畸形 JSON 清单当作普通失败而不是终止进程；预置取消信号必须在发起网络请求前结束扫描。UI 回归固定关闭面板为 RenderMesh 内部子窗口，只显示“直接退出”“最小化到系统托盘”和可勾选的“记住本次选择”，并验证默认焦点落在托盘选项；配置自检同时固定关闭行为 `ask/exit/tray` 的持久化。
44. 0.2.30 UI 回归要求服务器、任务、本机贡献和任务详情列表按当前客户区宽度重新分配列宽，并保证水平滚动位置为零；服务器表不再显示恒定为“仅空闲”的冗余调度策略列。原生竖向滚动条必须在 100 行溢出数据下仍保持隐藏，表头列宽之和必须完整覆盖客户区，内嵌细滚动条的轨道从表头下方开始且滑块始终落在轨道内。编辑渲染主机窗口使用行选择、全选/反选、加入/断开和右键操作，运行时探针要求全选只选可用节点、反选结果准确。渲染参数窗口的主要控件统一结束在与左边距对称的右边界；上传、参数、任务详情和单帧日志窗口均按当前主窗口或父详情窗口居中。退出确认等内部模态层打开后，强制调用后台列表刷新必须保持列表变更数为零、命中目标仍为模态层；关闭后只补一次合并刷新。托盘三种状态必须同时包含简化 RenderMesh 网格主体、白色 Fluent 状态角标和各自状态色，像素采样要求不再退化为单色块且三个状态图案存在明确差异。
45. 0.2.34 UI 回归禁止 `ListViewSubclassProc` 吞掉 `WM_ERASEBKGND`，并把深色、浅色列表的背景擦除发送到带哨兵色的内存 DC 后采样，要求空白区域确实被当前主题表面色覆盖。所有经 `ConfigureList` 初始化的列表必须带 `WS_CLIPSIBLINGS`；内部弹窗打开时强制后台列表立即重绘，弹窗仍须占据命中层且列表刷新必须延后。托盘算力共享回归必须在清空主开关更新区域后调用真实托盘切换处理，要求配置值改变、主开关产生新的局部重绘区域，并再次切换后原样恢复。设置页必须包含两个自绘启动开关，关闭开机自启时静默开关必须禁用；自动启动命令必须用引号包住当前 EXE 并携带 `--startup`，静默判定必须同时要求自启调用、开机自启配置和静默配置。临时 `--headless`、UI 回归与内置自检不得修改真实用户的 `Run` 注册项。`test-single-instance.ps1` 使用隔离数据目录启动静默实例，要求主窗口存在但不可见；再次普通启动同一数据目录必须在八秒内退出且返回 0，并把第一个实例恢复为可见，证明不会因 8391 端口占用启动失败。测试还强制首实例在持有单实例锁后延迟 4.5 秒创建窗口，第二实例必须等待命名就绪事件并在窗口出现后恢复它，覆盖开机启动较慢时的竞态。结构变更可以异步请求 `RDW_ERASE`，但不得在正常刷新路径使用 `RDW_UPDATENOW` 同步强制刷新；无变化快照不得触发重绘。100 行列表必须验证鼠标滚轮和内嵌滑块拖动都会改变首行，并在操作后继续同时移除 `WS_VSCROLL`、`WS_HSCROLL` 及原生滚动条可见状态。渲染参数布局仍使用客户区物理坐标，不能再次调用 DPI 换算。局域网更新自检必须模拟同一已信任节点在清单发现后切换首选网卡地址；下载仍使用清单中经过验证的原始端点并重新执行所有权证明，完成大小、SHA-256 和 PE 文件校验。生产发现循环同时向在线已信任地址单播签名状态，以兼容仍运行旧更新客户端的电脑。
46. 0.2.35 设置页回归通过真实窗口句柄向两个启动开关发送 `BM_CLICK`，要求开机自启立即变为选中、静默开关随之启用、静默选项也可切换，并把两个值写入隔离配置；自绘状态必须能经 `BM_SETCHECK/BM_GETCHECK` 往返，控件不得使用原生边框或整行自绘卡片。静默说明必须改为信息图标悬浮提示。当前版本和“检查更新”必须位于设置页；空更新结果要显示 RenderMesh 内部“当前已是最新版本”提示，且“知道了”会关闭。源码检查固定只保留启动后 4 秒的一次定时检查，不得重新注册 30 秒轮询。`test-lan-update-package.ps1` 启动真实 headless 服务，从生产 `/v1/update/manifest` 读取签名清单，再从生产 `/v1/update/package` 下载当前 EXE，要求 HTTP 200、长度与 SHA-256 完全一致，防止再次出现清单正常但包读取误判为 HTTP 503。
47. 0.2.36 UI 回归要求本机与远端空闲节点只显示“可接单”，非空闲节点只显示“使用中”，远端状态生成函数不得再读取或格式化 `idleSeconds`。软件配置下拉的屏幕坐标必须由统一定位函数生成，并在配置窗口 `WM_WINDOWPOSCHANGED` 和尺寸变化后重算；配置页必须提供“编辑配置”和“删除当前配置”，删除后持久化新的默认配置，且最后一个配置受到保护。
48. 0.2.37 `test-update-apply.ps1` 使用生产更新助手、真实 PE 替换目标和独立持锁句柄复现退出后的短暂 EXE 占用；旧实现必须稳定报告 `exit=24; pending=True; target_unchanged=True`。修复后要求助手等待 1.4 秒锁释放、原子替换目标、保留 `.previous`、启动替换版本并删除 `.pending`。发布门禁还检查助手必须复制自已经通过 SHA-256 校验的新 EXE，而不是旧安装版本；替换最终失败时必须重新启动原版本并记录具体 Win32 错误。
49. 0.2.39 更新回归让新版节点第一次 `/v1/prove` 固定返回 HTTP 503，旧实现必须以“worker failed fresh ownership proof”失败；修复后使用三个不同随机挑战进行有限重试，收到签名不合法的 200 响应仍立即拒绝。Blender 下载器纯逻辑自检覆盖官方目录/MSI/LTS 解析和 NVIDIA OptiX 配置脚本；真实 `--blender-catalog-report` 从官方镜像读取 5.2.0、5.1.2、5.0.1、4.5.12、4.4.3、4.3.2 Windows x64 MSI，并把 5.2 与 4.5 标为 LTS。真实 MSI `HEAD` 验证还覆盖 Blender 官方入口返回 302 地区镜像的行为：允许 HTTPS 跳转、拒绝 HTTPS 降级，最终仍以官方 `.sha256` 清单校验。
50. 0.2.40 UI 回归在设置页显示工作流蒙版后强制触发后台列表刷新和底层按钮重绘，要求蒙版继续占据命中最上层、刷新只被延迟一次且关闭后恢复；Blender 版本选择器必须使用固定高度的 owner-draw 项并保留完整版本数量。完整 UI 回归还要求滚动锚点、选中项和增量更新指标继续通过。
51. 0.2.41 用 `D:\05_Documents\Downloads\c24-28-896-动画-v002.blend`（4,028,576,691 字节）复现大型工程导入：0.2.40 单独读取耗时 42.22 秒，随后 GUI 会把 0–1170 完整范围传给本机低清预检渲染。新增结构回归先以 `Preflight must not render project frames locally` 返回 1；移除本机逐帧渲染后完整内置自检返回 0。真实 0.2.41 预检在 35.52 秒内完成元数据与素材扫描、实际渲染帧为 0，并准确报告 4 个 File Output 节点和 15 项缺失素材。
52. 0.2.42 用同一 4,028,576,691 字节工程再次执行真实无界面读取，返回 0，并得到帧范围 0–1170、步长 1、4 个 File Output 节点和 9 个输出项。大型工程工作流蒙版从自绘标题栏下方开始，读取时主窗口仍可拖动；持续超过一分钟才显示取消入口，复制、工程包写入、SHA-256、HTTP 分块和提交等待均响应同一取消信号。双 NodeApp 协议回归分别对普通 `.blend` 与含 `tex` 的确定性工程包先向 `/v3/cache/chunk` 写入 17 字节，确认 `/v3/cache/upload-status` 返回非零偏移，再经生产 `CreatePreparedDraft→ProjectSyncLoop→UploadProjectResumable` 续传、提交并验证最终缓存状态、总字节、元数据、源文件和 tex 内容；只能证明分块边界续传，不等同于 4 GB 弱网 soak。更新回归同时覆盖正常健康确认，以及新实例不发送健康回执、目标 EXE 又被额外锁住的失败路径：助手必须在 15 秒期限内重试回滚，最终目标 SHA-256 恢复为旧版且不遗留 `.pending`/`.failed-update`。
53. 0.2.43 工程存储回归在隔离配置目录中创建两帧任务，并额外写入 `cache` 与 `incoming` 数据；生产迁移函数必须把全部受管目录复制到新根目录、逐文件 SHA-256 校验、把数据库托管路径改写为 `storage://` 相对形式、原子切换 `project_storage_path`、清理旧受管目录并达到 100% 进度。随后用同一配置和数据库重新构造 NodeApp，任务工程必须从新根目录正常恢复。预置取消信号的第二次迁移必须返回失败、保留当前配置与新根目录完整数据。UI 回归要求设置页同时存在只读当前路径和“更改位置…”按钮。
54. 0.2.44 手动 Blender 路径回归创建隔离的 `blender.exe` 固件，要求统一配置函数将其规范化为绝对路径、更新当前渲染软件配置并在重载后继续被 `FindBlender` 找到；不存在的程序必须被拒绝。UI 回归要求设置页显示当前解析路径、只读输入框和“手动指定…”按钮。
55. 0.3.0 导入、体积与命令行回归，均在 2026-10-11 用最终 0.3.0 构建运行：
    - 导入耗时：`native/tests/make_large_fixtures.py` 生成 1,136,454,828 字节的自包含工程 `heavy.blend`，以及引用 6 个 4K OpenEXR（共 1,208,355,276 字节，位于 `textures\` 而非 `tex\`，因此新旧版都要打包）的 `textured.blend`。旧流程（读取、预检打包另存、复制进任务、计算哈希）分别为 8.9 秒和 8.7 秒，写盘 2,272,911,768 和 2,418,054,816 字节；新流程一次 `submit` 分别为 5.0 秒和 4.1 秒，只写入 1,136,454,828 和 1,209,027,408 字节，且不遗留 `preflight` 临时文件。测试机为 NVMe 固态盘，较慢磁盘上节省的时间会更多。`tex` 文件夹内的素材不再打包进 `.blend`，旧流程在这种工程上还会额外写一份工程包，新流程只从原文件写一次。
    - 体积：分发 `RenderMesh.exe` 从 3,556,864 字节降到 1,608,704 字节（−54.8%），发布 ZIP 约 0.8 MB；`RenderMesh-dev.exe` 为 2,207,744 字节。
    - `RenderMesh-dev.exe --self-test` 返回 0，新增单次快照断言（移动预检副本；没有副本时复制源工程并同时计算哈希；空 `tex` 目录；创建任务时取消必须回滚任务记录）和旧任务导入断言；自测用的 HTTP 夹具改为 `/v3/cache/upload-status` 与 `/v3/package`。
    - `verify-ui.ps1` 用 `RenderMesh-dev.exe` 通过（`UI_REGRESSION_OK`），导入线程断言改为“单次适配器预检、删除工作目录、先清除运行标志再通知界面”。
    - 分发的 `RenderMesh.exe` 通过 `test-large-project-transfer.ps1`、`test-lan-update-package.ps1`、`test-update-apply.ps1`、`test-single-instance.ps1`、`test-auto-pairing.ps1`、`test-peer-exchange.ps1`、`test-peer-state-refresh.ps1`、`test-decentralized-discovery.ps1`、`test-peer-idle-readiness.ps1` 和 `test-real-matching-id.ps1`。
    - `test-legacy-job-import.py` 用 Python `sqlite3` 生成 0.2.x 结构的 `jobs.db`（小行、需要溢出页的大行、多层 B 树内部页，以及最新提交仍在 WAL 中的数据库），要求 0.3.0 headless 启动后导入的每个任务记录与原始 payload 完全一致：`LEGACY_JOB_IMPORT_OK`。
    - `test-missing-asset-override.ps1` 改用 `inspect --json`：首次检查退出 1 并列出缺失项，`--ignore-warnings` 后退出 0 且保留缺失清单：`IGNORE_ALL_WARNINGS_OK`。
    - `test-cli-import.py` 用真实 Blender 5.2 生成带 `tex` 文件夹、`tex` 外贴图和缺失贴图的工程，只通过命令行完成 `version`、`help`、未知命令（退出 2）、节点未运行时的 `status`（退出 3）、`serve`、`inspect`、`submit --wait`、`jobs`、`test`、`start`、`job`、`stop`、`resume`、`retry`（没有失败帧时退出 1）和 `delete`；要求无需打包的工程原样复制、`tex` 素材保持 `//tex/...` 相对路径并随工程包传输、`tex` 外的贴图被打包、`preflight` 临时目录清空、两个任务用真实 Blender 渲染出全部帧：`CLI_IMPORT_OK`。

测试使用 Blender 官方便携构建 `5.2.0 LTS fbe6228777e7`；该大型测试工具不包含在发布 ZIP 中。

## 便携性

- Release EXE 使用 llvm-mingw 静态构建（`-Os`、LTO、`--gc-sections`、`--icf=safe`、去除符号），不再链接 SQLite 和 cpp-httplib。
- 导入表只允许 Windows 系统 DLL。
- 发布 ZIP 在新的临时目录解压后重新运行完整自检和短时 headless 启动。
