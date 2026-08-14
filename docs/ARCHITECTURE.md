# RenderMesh 架构

## 运行模型

每个 EXE 内含同一套 Origin、调度器和 Worker：

```text
选择 .blend
    │
    ▼
Blender 预检/打包 ──缺素材或无法打包──► 阻止提交
    │
    ▼
按帧生成任务 ──► 可信且已开启共享的在线 Worker ──► Blender 后台渲染
    │                         │
    └────没有可用 Worker─────┘                    │
              本机渲染 ◄─────────────────────────┘
                         单帧结果回传 Origin
```

UDP 8390 广播协议版本、随机 nonce、节点 ID、节点自身 IPv4 地址列表、名称、HTTP 端口、共享状态、CPU/GPU 型号、任务数和空闲时间。协议 v2 把名称、空闲时间和硬件字段全部纳入 HMAC-SHA256 签名，协议 v3 再显式声明并签名工程缓存能力，协议 v4 声明并签名五帧任务包能力，协议 v5 声明并签名多文件结果包能力，协议 v6 声明并签名任务包远端停止能力；调度器不会只凭旧版本号推断新能力。升级过渡期仍保留旧 CPU 字段并固定为 100，仅用于维持 v1/v6 签名和旧节点解析兼容，不参与调度或限速。升级过渡期同时发送字段与签名规则保持旧版不变的 v1 兼容信标，使尚未升级的节点仍能发现新版节点。新版 Origin 对没有 v6 停止能力的旧 Worker 自动降为一帧包，避免停止任务后旧 Worker 继续跑完额外四帧。新版接收端收到同节点的 v1 信标时会保留已由新版信标签名验证的名称、空闲时间、硬件及能力信息；只有始终未发送新版信标的旧节点仍沿用旧版较弱的字段保证。程序枚举所有已启用的 IPv4 网卡，根据前缀分别计算定向广播地址；`/31`、`/32` 等无广播网卡会跳过。发现签名始终使用内部随机集群凭证，不使用或广播六位邀请码。接收端要求 UDP 实际来源 IP 位于签名地址列表中，无效包不会覆盖已认证节点。广播不可用时，用户可保存一个手动 `IPv4:端口`；程序周期性请求 `/health` 并完成 `/v1/prove` 挑战，验证通过后进入同一个调度池。Origin 只向在线、验证有效、已开启共享且当前无任务的节点派发；Worker 只在达到本机闲置阈值时接受工程兼容检查和远端任务。远端接单后若节点离线，或重新报告为空闲却没有回传结果，Origin 会在 20 秒宽限期后将帧重新排队。

TCP 8391 提供 `/health`、`/v1/prove`、`/v1/task`、`/v1/result`、`/v2/pair`、`/v2/cache/status`、`/v2/cache`、`/v2/task`、`/v3/package` 和 `/v3/cancel`。六位邀请码只是人工输入的配对码：新节点进入待配对状态后，用户必须明确填写一台已加入集群的可信局域网 `IPv4:端口`；服务端按来源 IP 限制尝试次数，验证成功才把内部随机集群凭证交给新节点。邀请码不会出现在发现广播中。

Origin 对打包完成的 `.blend` 计算流式 SHA-256。协议 v2 先以哈希、Blender 版本、渲染引擎和 GPU 要求查询 Worker；只有缓存不存在或兼容条件变化时才上传工程，随后每帧任务只发送元数据。缓存上传以 1 MiB 块流式写入 `.receiving`，完成后原子替换并重新计算完整 SHA-256，哈希不符即删除拒绝。协议 v1 仍保留整文件逐帧上传，供升级过渡期的旧节点使用。

预检产生的 packed `.blend` 复制进任务目录时使用 Windows 原生复制并识别 `ERROR_SHARING_VIOLATION`、`ERROR_LOCK_VIOLATION`、短暂 `ERROR_ACCESS_DENIED`、`ERROR_BUSY` 与 mapped-file 占用；每 50 ms 重试，最长 5 秒。它只覆盖提交阶段的瞬时文件竞争，不把永久权限、路径或磁盘错误转换为成功。

Worker 的兼容性检查不先打开提交的 `.blend`，而是在空白 Blender 后台进程中核对主/次版本、渲染引擎和 GPU，避免旧 Blender 因无法打开新工程而只留下不可操作的笼统错误。插件安装、启用和版本清单不参与兼容判断；协议 v6 仍向旧节点发送空插件列表，接收端也忽略旧任务中的非空插件字段。Cycles GPU 必须报告已启用的非 CPU Cycles 设备；Eevee 必须成功建立真实 GPU 渲染上下文并完成 1×1 空白场景探针。兼容性、磁盘容量或协议能力等确定性拒绝在当前进程内按“任务+节点”记忆，该任务不再冲击同一节点；Worker 忙、电脑恢复使用或网络抖动只退避 5 秒。两类失败都会立即继续尝试其他 Worker，若没有可用节点则由本机兜底；新任务或应用重启后会重新探测，以便识别刚升级的 Blender/GPU 环境。拒绝原因写入持久日志。

每个 Blender 可执行文件在当前 RenderMesh 进程内首次使用前都要通过 readiness bootstrap：以 `--background --factory-startup --disable-autoexec` 启动，不打开工程；只有在 Blender 自己解析出的配置目录中不存在 `userpref.blend` 时才保存默认偏好，并以原子就绪文件确认完成。启动超过 15 秒会由 Windows Job Object 终止，避免首次配置界面或安装渠道启动器永久占住 Worker。随后预检、兼容检查和正式渲染仍读取正常用户配置；Cycles GPU 只在当前子进程内选择一个后端，不持久化覆盖用户偏好。

正式渲染必须在进度清单中完成全部请求帧，或明确处于“电脑恢复使用后完成当前帧”的部分完成状态；仅有 Blender 退出码 0 不能视为成功。远端 Blender 若在 10 秒内正常退出但没有生成任何请求帧，Worker 会清理本次空结果并自动重启同一包一次。再次失败会把非空错误、日志尾部和 failed 状态回传 Origin，Origin 同步写入任务详情与持久日志。

具备任务包能力的节点按提交时的真实帧序列每五帧分组，例如步长为 2 的 `1,3,5,7,9,11,13` 会成为 `1,3,5,7,9` 与 `11,13` 两包。包内每帧拥有独立 task ID 和 attempt token，Worker 只启动一次 Blender；Blender 每完成一帧便原子更新清单，宿主进程以 100 ms 间隔消费新增帧，立即打包、回传并使 Origin 更新该帧状态，不等待整包退出。Worker 对外报告 package ID，使租约恢复不会把同包中等待的后续帧误判为丢失。旧节点自动降级为单帧协议。

Blender 5.2 的活动场景允许第 0 帧，RenderMesh 会正确读取、显示和本机渲染它。协议 v6 没有“支持第 0 帧”的能力位，为避免升级期间旧 Worker 把它拒绝，第 0 帧暂由 Origin 本机兜底；其余帧仍可正常远程分发。

选择工程时，独立的无界面检查读取活动场景 `frame_start`、`frame_end` 与 `frame_step` 并自动填写提交范围。预检递归记录活动场景及其节点组内 File Output 节点的稳定 ID、名称、场景名、节点索引路径、文件名模式、AOV/输出项、是否要求产出和原始基础目录，并记录每帧的主输出绝对路径；输出项兼容 Blender 5.2 `file_output_items` 与旧版 `file_slots`/`layer_slots`。Worker 按节点路径定位并把 File Output 的 `base_path`/`directory` 重定向到任务沙箱，同时用原始 `.blend` 名称解析 `{blend_name}`。渲染每帧后快照实际生成文件；若文件名本身没有帧号，逻辑输出名补六位帧号，避免固定文件名被下一帧覆盖。启用工程合成且自身及父节点组均未 muted 的每个 File Output 节点都必须实际生成至少一个文件，否则该帧失败。muted 节点或关闭 `scene.render.use_compositing` 时仍保留目标条目以兼容旧 v6 Worker 的顶层节点计数，但不要求产物。结果采用 `RMBNDL1` 多文件包回传；Origin 只接受 `primary` 或已记录节点 ID 下的相对路径，先完整解包与租约复核，再在各目标目录暂存、备份旧文件并发布，失败时回滚。远端不能提供任意 Origin 绝对路径。

服务端单请求上限为 100 GiB，并在接收前检查剩余磁盘空间。上传使用可中止的数据提供器，入站流也逐块检查退出信号，并配合 5 秒无数据超时，退出不会被失联节点或持续大文件传输长期卡住。每个 Worker 原子地保留一个执行槽，缓存兼容性检查也占用该槽，避免并发请求绕过“单 Blender 进程”限制。每次派发都有新的随机 attempt token；只接受当前租约的结果，掉线重派后的旧 Worker 无法覆盖新结果。

## 持久化

默认数据目录：

```text
%LOCALAPPDATA%\RenderMesh\
  config.json
  jobs.json
  contributions.json
  logs\RenderMesh-YYYY-MM-DD.log
  tools\preflight.py
  tools\compatibility.py
  jobs\<job-id>\source.blend
  jobs\<job-id>\outputs\
  cache\projects\<sha256>\source.blend
  cache\projects\<sha256>\cache-entry.json
  cache\projects\<sha256>\last-used
  incoming\<task-id>\source.blend
  incoming\<task-id>\outputs\
  failed-results\<task-id>.txt
```

程序重启时，把未完成的 Assigned/Dispatching/Rendering 帧恢复为 Pending；完成结果和错误记录保留。Worker 在缓存上传、接单和任务结束时刷新 `last-used`；后台只在没有活跃任务时清理最后使用超过七天的工程目录。协议 v2 的逐帧临时目录只保存输出并在回传后删除；协议 v1 的临时工程也按旧逻辑随帧删除。

`jobs.json` 同时记录源 `.blend` 路径、每帧开始/结束时间和 Stopped 状态。`contributions.json` 最多保留 5000 个 Worker 任务包历史，包含来源、分配帧、已回传帧、状态与时间；两者均使用独立临时文件原子替换。

## Blender 进程边界

Worker 通过参数数组启动 Blender，不经过 shell。正式渲染使用：

```text
blender --background --disable-autoexec source.blend
        --python render-package.py -- <输出目录> <进度清单> <停止标记> <帧数组>
```

输出格式与是否附加扩展名由 `.blend` 工程设置决定，RenderMesh 不再传入 `--render-format` 覆盖工程。预检记录格式、扩展名和 `use_file_extension`；关闭自动扩展名的工程会按无扩展名文件接收。

本机用户自己提交并由本机兜底执行的任务不受闲置状态影响。所有别人发来的共享任务，无论 Eevee、Cycles GPU 还是 Cycles CPU，都只能在 Worker 达到闲置阈值后接单；检测到键鼠恢复后写入停止标记，Blender 完成当前帧才退出，已完成帧正常回传，包内其余帧请求 Origin 重派。应用退出仍会立即终止 Blender。Cycles GPU 还要求 Worker 检测到已启用的非 CPU Cycles 设备。RenderMesh 不创建 CPU rate-control Job Object，也不对 Blender 设置 CPU 百分比硬上限；它只尝试把 Blender 放入启用 `KILL_ON_JOB_CLOSE` 的生命周期 Job，失败时降级为直接管理主进程，避免受管电脑因 Job 嵌套限制而拒绝渲染。

## 节点活动展示

协议 v7 在既有发现心跳中增加并签名 Worker 当前作业名、帧包、完成数和渲染阶段。服务器页因此可以在“已分配”列显示每台电脑正在渲染的作业和帧进度，不传输工程绝对路径。手动连接会用随机 challenge 对同一份活动快照单独做 HMAC 验证，不能把一次认证证明拼接到被篡改的 `/health` 数据；活动期间以 300ms 合并轮询，空闲时回到 1 秒。旧节点继续退化为仅显示“正在渲染”。

## 生产化后续

1. 增加独立心跳与可续期租约，缩短当前基于发现广播的 20 秒故障恢复时间。
2. 增加 AD/SSO、设备证书或 mTLS、项目 ACL、审计和磁盘配额。
3. 增加签名安装包、自动升级和 IT 集中下发的防火墙规则。
4. 增加按 GPU 厂商的空闲策略、显存和温度保护。
