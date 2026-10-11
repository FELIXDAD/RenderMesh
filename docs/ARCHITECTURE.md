# RenderMesh 架构

## 运行模型

每个 EXE 内含同一套 Origin、调度器和 Worker：

```text
选择 .blend
    │
    ▼
适配器预检/按需打包 ──素材/插件/节点/引擎警告──► 提醒 ──明确“无视警告”──► 直接准备任务
    │
    └──源工程无法读取或复制────────────────────────────────────────► 无法提交
    │
    ▼
按帧生成任务 ──► 可信且已开启共享的在线 Worker ──► Blender 后台渲染
    │                         │
    └────没有可用 Worker─────┘                    │
              本机渲染 ◄─────────────────────────┘
                         单帧结果回传 Origin
```

## 渲染软件适配器

与具体 DCC 相关的代码集中在 `RenderAdapter` 接口（`native/src/Render.hpp`）之后：工程扩展名、可执行文件定位与配置、预检/按需打包（`Prepare`，含只检查不另存的 `inspectOnly` 模式）、Worker 兼容性检查、任务包渲染和预览转换。调度、传输、缓存、任务记录、界面和命令行只处理与软件无关的 `PreflightReport`、任务包和结果清单。`RenderJob` 与 `WorkItem` 携带 `software` 字段（当前为 `blender`），节点用 `X-RM-Software` 请求头声明任务所需软件，旧节点忽略该头。`BlenderAdapter.cpp` 是第一个实现；以后接入 C4D（Octane、Redshift 等渲染器插件）时新增一个适配器并在 `RenderAdapters()` 中登记，不需要修改调度和传输层。

UDP 8390 广播协议版本、随机 nonce、节点 ID、节点自身 IPv4 地址列表、名称、HTTP 端口、共享状态、CPU/GPU 型号、任务数和空闲时间。协议 v2 把名称、空闲时间和硬件字段全部纳入 HMAC-SHA256 签名，协议 v3 再显式声明并签名工程缓存能力，协议 v4 声明并签名五帧任务包能力，协议 v5 声明并签名多文件结果包能力，协议 v6 声明并签名任务包远端停止能力；调度器不会只凭旧版本号推断新能力。当前发现签名还包含由内部随机集群凭证与六位匹配 ID 计算的不可逆绑定标签；标签不暴露六位数字，但会让当前 ID 不同的节点即使残留同一旧凭证也无法互相认证。此安全收紧要求两端均升级到当前版本。程序枚举所有已启用的 IPv4 网卡，根据前缀分别计算定向广播地址；`/31`、`/32` 等无广播网卡会跳过。接收端要求 UDP 实际来源 IP 位于签名地址列表中，无效包不会覆盖已认证节点。广播不可用时，用户可保存一个手动 `IPv4:端口`；程序周期性请求 `/health` 并完成 `/v1/prove` 挑战，匹配 ID 绑定和内部凭证均验证通过后进入同一个调度池。Origin 只向在线、验证有效、已开启共享且当前无任务的节点派发；Worker 只在达到本机闲置阈值时接受工程兼容检查和远端任务。远端接单后若节点离线，或重新报告为空闲却没有回传结果，Origin 会在 20 秒宽限期后将帧重新排队。

协议 v9 进一步签名 `resumable_cache` 与 `max_package_frames=20` 能力；协议 v10 正式引入并签名匹配 ID 绑定；协议 v11 再签名 `blender_ready` 并用独立 challenge 证明手动节点的 Blender 可用状态；协议 v12 声明并签名认证节点交换能力；协议 v13 声明 `direct_status`，用一次性 challenge 对 `/health` 完整状态快照做 HMAC；协议 v14 再声明并签名由 Worker 按自身阈值计算的 `idle_ready`；协议 v15 声明并签名 `frame_start`，表示 Worker 会为包内每一帧发送真实开始事件。v15 仍发送一份完整认证的 v11 兼容发现报文，允许逐台升级；新任务只向检测到 Blender、已完成该作业工程同步、未被用户踢出作业队列且已报告达到自身空闲阈值的新版参与节点派发。未声明 `frame_start` 的旧节点仍可接单，但 Origin 会把该节点的有效包大小限制为一帧，避免多帧长期停留在“排队中”。

TCP 8391 由程序内置的 HTTP/1.1 层提供 `/health`、`/v1/prove`、`/v1/peers`、`/v1/frame-start`、`/v1/frame-log`、`/v1/result`、`/v1/update/manifest`、`/v1/update/package`、`/v2/pair`、`/v2/cache/status`、`/v3/cache/upload-status`、`/v3/cache/chunk`、`/v3/cache/commit`、`/v3/package`、`/v3/cancel`、`/v4/job-cleanup`，以及只接受本机回环请求的命令行控制接口 `/v5/local/*`。六位匹配 ID 是人工输入的配对码：节点修改 ID 后立即轮换内部随机凭证、清空已认证节点并进入待匹配状态。协议 v11 会额外广播不受信的配对候选端点与 ID 派生标签；待匹配节点只用它定位 `/v2/pair`，不会把候选直接放入服务器或任务池。两个全新待匹配节点由 node ID 排序较小的一端发起请求，避免并发交换凭证；成功后双方统一随机凭证，之后只接受带完整 HMAC 签名的正常发现。广播不可用时仍可手动填写 `IPv4:端口`。

每个节点在全部可用 IPv4 网卡上同时发送定向广播和 `239.255.83.90`、TTL=1 的局域网组播，直接公布自己的地址和签名状态，因此网络中不存在发现协调器。协议 v12 的 `/v1/peers` 只接受内部集群凭证，并用随机 challenge 对节点 ID 和最多 128 个新鲜端点的完整目录做 HMAC；它在 UDP 被局部拦截时仅提供“待验证地址”。每个首次出现的端点仍必须独立通过匹配 ID 绑定、活动/能力证明和 `/v1/prove` 才进入服务器与任务池。协议 v13 对目录或手动地址认识的端点每秒直接轮询实际 `/health`，第三方目录绝不能更新该端点的 `lastSeen`、`assigned`、`task_id` 或活动状态。测试机退出后，其他节点继续通过自己的广播、组播和直接状态握手形成网格。

Origin 在写入任务快照的同时计算 SHA-256。协议 v9 先查询远端已确认偏移，再在同一条 keep-alive 连接上以 8 MiB 分块上传；每块携带 SHA-256 并在接收时校验，分块写入不单独刷盘。提交时，在同一接收进程内从偏移 0 连续校验完成的文件只刷盘一次后原子发布；含有更早会话 partial 数据的文件才重新计算完整 SHA-256。网络中断会保留 partial 文件供下次续传，进度持久化与界面通知最多每 500 ms 一次，终态立即落盘；同一工程哈希使用独立互斥锁，不会在整文件哈希时阻塞其他工程。七天未继续的 partial 数据会自动清理。0.3.0 删除了协议 v1/v2 的整文件上传和单帧派发端点（`/v1/task`、`/v2/cache`、`/v2/task`）：v10 信任层早已不会把缺少匹配 ID 绑定的旧节点加入调度池，所有可调度节点都使用 v9 断点续传与 `/v3/package`。`/v2/cache/status` 保留给 0.2.x 节点查询缓存。

任务快照只写一遍：预检另存的副本与任务目录同卷时直接移动，再读一遍计算 SHA-256；跨卷时一边复制一边计算 SHA-256。预检没有需要打包或改写的内容时 Blender 不另存副本，快照直接从原工程复制并同时计算哈希。读取源文件时识别 `ERROR_SHARING_VIOLATION`、`ERROR_LOCK_VIOLATION`、短暂 `ERROR_ACCESS_DENIED`、`ERROR_BUSY` 与 mapped-file 占用；每 50 ms 重试，最长 5 秒。它只覆盖提交阶段的瞬时文件竞争，不把永久权限、路径或磁盘错误转换为成功。

如果源 `.blend` 同级存在 `tex` 目录，预检不把其中的素材打包进 `.blend`，只把指向 `tex` 的绝对路径改写为 `//tex/...`（`relative_remap=False` 保证另存副本中的相对路径仍以原工程目录为基准）。发起端随后把 `source.blend` 和 `tex` 内的原始文件按逻辑路径排序，一次流式写入 `RMPKG1` 工程包并同时计算 SHA-256，不创建硬链接或中间副本。本机渲染需要该工程时才在任务目录中展开一次。包只允许 `source.blend` 与 `tex/` 下的规范相对路径，并拒绝重解析点、重复路径、权限不足和越界大小；空 `tex` 目录也会在 Worker 还原。整个包计算一个 SHA-256，沿用协议 v9 的分块断点续传；Worker 提交缓存时在哈希校验后安全解包为同层级 `source.blend + tex`，因此 Blender 的相对引用不变。不含 `tex` 的工程继续使用单文件路径；带工程包的任务只选择应用版本不低于 0.2.29 的参与节点。

作业创建时保存一组参与节点 ID，但它不是一次性快照。任务列表永久提供“编辑渲染主机”，可把后来发现的受信、共享、Blender 就绪节点加入，也可移除现有节点。变更先在 stage gate 内复制并持久化作业状态，再发布给调度器；新增节点进入工程同步，只有缓存完成后才可派发，移除节点会停止后续选择但不会破坏已经完成的帧。运行中的远端包仍通过既有停止/租约栅栏收尾或取消。

预检只启动一次 Blender：同一个脚本读取场景设置、帧范围、输出和合成器信息，检查外部文件，并在需要时打包和另存。它把外部文件分为四类：缺失；位于 `tex` 内（由工程包传输）；可打包（`tex` 之外的图像、字体、声音、链接库和体积）；不可打包（影片、序列、缓存等，仅报告，并把相对路径改为绝对路径以便本机仍可读取）。发现素材、插件或节点问题时先返回可见警告，不创建任务。用户显式选择“无视警告”后，第二次准备会跳过全部预检警告和未定义节点扫描；只要源工程仍可读取并复制，就直接创建待上传任务。发起端预检不渲染任何提交帧，真实出图兼容性由只在参与 Worker 上运行的测试帧确认。该选择写入后台日志，最终渲染结果由提交者负责确认。

Worker 的兼容性检查不先打开提交的 `.blend`，而是在空白 Blender 后台进程中核对主/次版本、渲染引擎和 GPU，避免旧 Blender 因无法打开新工程而只留下不可操作的笼统错误。插件安装、启用和版本清单不参与兼容判断；协议 v6 仍向旧节点发送空插件列表，接收端也忽略旧任务中的非空插件字段。Cycles GPU 必须报告已启用的非 CPU Cycles 设备；Eevee 必须成功建立真实 GPU 渲染上下文并完成 1×1 空白场景探针。兼容性、磁盘容量或协议能力等确定性拒绝在当前进程内按“任务+节点”记忆，该任务不再冲击同一节点；Worker 忙、电脑恢复使用或网络抖动只退避 5 秒。两类失败都会立即继续尝试其他 Worker，若没有可用节点则由本机兜底；新任务或应用重启后会重新探测，以便识别刚升级的 Blender/GPU 环境。拒绝原因写入持久日志。

每个 Blender 可执行文件在当前 RenderMesh 进程内首次使用前都要通过 readiness bootstrap：以 `--background --factory-startup --disable-autoexec` 启动，不打开工程；只有在 Blender 自己解析出的配置目录中不存在 `userpref.blend` 时才保存默认偏好，并以原子就绪文件确认完成。启动超过 15 秒会由 Windows Job Object 终止，避免首次配置界面或安装渠道启动器永久占住 Worker。随后预检、兼容检查和正式渲染仍读取正常用户配置；Cycles GPU 只在当前子进程内选择一个后端，不持久化覆盖用户偏好。

正式渲染必须在进度清单中完成全部请求帧，或明确处于“电脑恢复使用后完成当前帧”的部分完成状态；仅有 Blender 退出码 0 不能视为成功。远端 Blender 若在 10 秒内正常退出但没有生成任何请求帧，Worker 会清理本次空结果并自动重启同一包一次。再次失败会把非空错误、日志尾部和 failed 状态回传 Origin，Origin 同步写入任务详情与持久日志。

具备任务包和 `frame_start` 能力的节点按提交时的真实帧序列每包 1–20 帧分组，默认 5 帧；例如默认设置下，步长为 2 的 `1,3,5,7,9,11,13` 会成为 `1,3,5,7,9` 与 `11,13` 两包。包内每帧拥有独立 task ID 和 attempt token，Worker 只启动一次 Blender。Origin 在包接单时把所有帧置为 `Assigned/排队中`，不写 `started_at`；批处理脚本在每帧调用 `bpy.ops.render.render` 前后分别把 Unix 毫秒时间原子写入清单。宿主读取到某帧开始后，先通过 `/v1/frame-start` 携带 task ID、attempt token 和真实开始时间，将且仅将该帧切到 `Rendering/渲染中`；Blender 每完成一帧便更新清单，宿主立即打包，并把该帧真实起止时间随 `/v1/result` 一起回传。结果和日志接口同时接受同一有效租约的 `Assigned` 与 `Rendering`，但拒绝过期 attempt。这样每帧时长不包含包内前序帧、排队、打包或网络上传。Worker 对外报告 package ID，使租约恢复不会把同包中等待的后续帧误判为丢失。未声明该能力的旧节点被限制为单帧包，并在派发时沿用近似 `Rendering` 状态；最终结果仍会覆盖为它提供的真实起止时间。

Worker 为每个活动任务包维护独立的首帧中止信号。`/v3/cancel` 到达时，如果活动清单尚未报告任何帧进入渲染，RenderMesh 会请求中止 readiness 或正在打开 `.blend` 的 Blender 子进程；每帧 Python 从写入 `started_at` 到完成产物快照和 `ended_at` 清单期间持有一个命名 Windows mutex，宿主只有在能取得该锁并持续持有到子进程退出时才执行硬中止。因此即使停止恰好落在开始事件的轮询窗口，也不会杀掉已经开始的帧。如果当前帧已经真正开始，则只写入 stop marker，让 Blender 完成并回传当前帧后跳过包内后续帧。这将“停止加载中的任务”和“完成正在渲染的当前帧”分成了两个明确阶段。

单帧开始由独立的 20 ms 清单监视线程读取，不与结果包/AOV 打包上传共享阻塞路径；主进度线程在处理同帧完成前仍会通过同一串行化栅栏确认开始回调已经发送或尝试，保证极快 Eevee 帧也不会先到结果、后到开始。Worker 用当前渲染帧号更新活动状态，旧帧完成回调不能覆盖已经开始的下一帧。

Blender 5.2 的活动场景允许第 0 帧，RenderMesh 会正确读取、显示和本机渲染它。协议 v6 没有“支持第 0 帧”的能力位，为避免升级期间旧 Worker 把它拒绝，第 0 帧暂由 Origin 本机兜底；其余帧仍可正常远程分发。

选择工程时，预检报告中的活动场景 `frame_start`、`frame_end` 与 `frame_step` 会自动填写提交范围。主窗口用一个覆盖整个客户区、位于全部兄弟控件上方的输入拦截层展示两阶段进度（读取并检查工程、创建任务；第二阶段按已写入字节显示进度）；它消费客户区鼠标和键盘输入，但不拦截主窗口关闭，关闭会通过既有取消信号等待 Blender 安全退出。预检递归记录活动场景及其节点组内 File Output 节点的稳定 ID、名称、场景名、节点索引路径、文件名模式、AOV/输出项、是否要求产出和原始基础目录，并记录每帧的主输出绝对路径；输出项兼容 Blender 5.2 `file_output_items` 与旧版 `file_slots`/`layer_slots`。Worker 按节点路径定位并把 File Output 的 `base_path`/`directory` 重定向到任务沙箱，同时用原始 `.blend` 名称解析 `{blend_name}`。渲染每帧后快照实际生成文件；若文件名本身没有帧号，逻辑输出名补六位帧号，避免固定文件名被下一帧覆盖。启用工程合成且自身及父节点组均未 muted 的每个 File Output 节点都必须实际生成至少一个文件，否则该帧失败。muted 节点或关闭 `scene.render.use_compositing` 时仍保留目标条目以兼容旧 v6 Worker 的顶层节点计数，但不要求产物。结果采用 `RMBNDL1` 多文件包回传；Origin 只接受 `primary` 或已记录节点 ID 下的相对路径，先完整解包与租约复核，再在各目标目录暂存、备份旧文件并发布，失败时回滚。远端不能提供任意 Origin 绝对路径。

服务端单请求上限为 100 GiB，并在接收前检查剩余磁盘空间。上传使用可中止的数据提供器，入站流也逐块检查退出信号，并配合 5 秒无数据超时，退出不会被失联节点或持续大文件传输长期卡住。每个 Worker 原子地保留一个执行槽，缓存兼容性检查也占用该槽，避免并发请求绕过“单 Blender 进程”限制。每次派发都有新的随机 attempt token；只接受当前租约的结果，掉线重派后的旧 Worker 无法覆盖新结果。

## 持久化

本机配置目录固定为：

```text
%LOCALAPPDATA%\RenderMesh\
  config.json
  jobdb\<job-id>.json
  contributions.json
  logs\RenderMesh-YYYY-MM-DD.log
  tools\initialize-blender.py
  updates\
```

工程存储目录由 `config.json` 的 `project_storage_path` 指向，默认与本机配置目录相同，可在设置页修改：

```text
<project_storage_path>\
  jobs\<job-id>\source.blend
  jobs\<job-id>\project.rmp            （含 tex 的工程包）
  jobs\<job-id>\project\source.blend   （本机渲染时展开）
  jobs\<job-id>\outputs\
  preflight\<random>\                  （单次预检的临时目录，结束即删除）
  cache\projects\<sha256>\source.blend
  cache\projects\<sha256>\cache-entry.json
  cache\projects\<sha256>\last-used
  cache\uploads\<sha256>\source.blend.partial
  incoming\<task-id>\source.blend
  incoming\<task-id>\outputs\
  failed-results\<task-id>.txt
```

修改工程存储目录时，NodeApp 先停止所有写入线程，把受管目录复制到目标同卷临时目录并逐文件比较 SHA-256。任务记录先把托管路径序列化为 `storage://` 相对路径，随后由 `config.json` 的原子替换完成唯一切换；发布成功后内存路径同步换根并清理旧目录。取消、复制失败或校验失败不会改变配置指针。

`config.json` 同时保存 `render_software_profiles` 与 `last_render_software_profile_id`。每个配置包含稳定 ID、显示名称、渲染系统、渲染软件和本机安装路径。设置页的手动 Blender 路径入口直接更新当前配置的 `install_path`，然后交由同一 `FindBlender` 解析链供工程检查、预检、预览和 Worker 渲染使用。开始测试时，当前配置 ID、名称和可执行文件会写入作业元数据，本机任务包使用作业固化的路径，之后切换默认配置不会改变已经进入队列的作业。

`close_behavior` 取 `ask`、`exit` 或 `tray`。选择托盘时主窗口隐藏但 NodeApp、发现、HTTP、同步和 Worker 线程继续运行；托盘图标由当前错误/活动/空闲快照着色，提示文字和右键菜单只读取同一不可变 UI 快照，不启动第二套状态源。

程序重启时，把未完成的 Assigned/Dispatching/Rendering 帧恢复为 Pending；完成结果和错误记录保留。Worker 在缓存上传、接单和任务结束时刷新 `last-used`；后台只在没有活跃任务时清理最后使用超过七天的工程目录。协议 v2 的逐帧临时目录只保存输出并在回传后删除；协议 v1 的临时工程也按旧逻辑随帧删除。

`jobdb\<job-id>.json` 记录源工程路径、渲染软件、所选软件配置、每帧开始/结束时间、Stopped 状态、终态时间、清理时间和优先级；`contributions.json` 最多保留 5000 个 Worker 任务包历史，包含来源、分配帧、已回传帧、状态与时间，并使用独立临时文件原子替换。界面清理贡献历史时按 package ID 删除所选元数据并原子替换文件，不触碰工程或输出。

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

## 单帧 Blender 日志

一个任务包仍只启动一次 Blender，并按任务设置连续渲染 1–20 帧（默认 5 帧）。批处理脚本在每帧前后向同一 stdout/stderr 写入边界标记，Core 从 `render-package.log` 切出最多 4 MiB 的单帧纯文本日志；首帧同时保留 Blender 启动和工程加载输出。Worker 通过带集群令牌、task ID 和 attempt token 的 `/v1/frame-log` 原子回传日志快照，Origin 在接收前后都复核租约，旧 Worker 忽略 `X-RM-Frame-Logs` 后仍可正常渲染。日志路径随帧元数据写入任务记录，和该任务的托管数据一起按七天策略清理。

## 作业历史、优先级与清理

每个任务是 `jobdb\<job-id>.json` 中的一个记录，原子替换写入；内容指纹不变时跳过写入，因此刷新进度不会反复重写所有任务。0.3.0 首次启动时把 0.2.x 的 `jobs.db`（SQLite，含仍在 WAL 中的提交）或更早的 `jobs.json` 一次性导入：记录先写入 `jobdb.importing`，再用一次重命名发布，中断后会重新导入；旧文件改名为 `.imported` 保留。导入使用只读的精简 SQLite 文件格式读取器，程序不再包含 SQLite 引擎。如需回退到 0.2.x，先把 `jobs.db.imported` 改回 `jobs.db`，否则旧版看不到任务历史。任务历史不按数量截断，列表按提交时间倒序显示；调度按优先级降序、同优先级提交时间升序选择下一任务包，不抢占正在渲染的帧。

任务进入 Completed、Failed 或 Stopped 终态满七天后保留历史行并显示“（过期）”，只清理 RenderMesh 托管目录和远端缓存，不删除原始 `.blend` 或已经发布到用户目录的结果。协议 v8 显式声明并签名 `job_cleanup` 能力；`/v4/job-cleanup` 只接受内部集群凭证、合法 Job ID、工程 SHA-256 和幂等 operation ID。Origin 为离线节点持久保留清理请求，节点重连并声明该能力后继续执行；Worker 渲染中返回 409 让 Origin 重试，空闲时只删除自身数据目录内匹配的任务临时目录和工程缓存。

## 命令行与本机控制接口

`RenderMesh.exe <命令>` 由 `Cli.cpp` 处理，帮助文本遵循 ASD-STE100 简化技术英语。`inspect` 在命令进程内调用适配器的只检查模式，不需要节点；`submit` 在命令进程内完成预检并把报告交给节点，其余命令（`status`、`jobs`、`job`、`test`、`start`、`stop`、`resume`、`retry`、`delete`）只调用正在运行的节点的 `/v5/local/*`。命令从 `--data-dir` 下的 `config.json` 读取端口和内部凭证，请求带 `X-RM-Local-Token`，节点只接受来自 `127.0.0.1` 的请求。`--json` 输出单行 JSON；退出码 0 成功、1 操作失败、2 命令或参数错误、3 本机节点未运行。`serve` 以无窗口方式运行同一 NodeApp，与桌面程序共用单实例锁。

## 程序变体

`build.ps1` 默认生成分发用 `RenderMesh.exe`；`-DevTools` 定义 `RM_DEV_TOOLS` 并生成 `RenderMesh-dev.exe`，额外包含内置自检、UI 度量报告、开发命令（预览、渲染、兼容性、版本目录）和测试渲染桩。Release 构建使用 `-Os`、LTO、`--gc-sections` 和 `--icf=safe`，并去除符号。

## 生产化后续

1. 增加独立心跳与可续期租约，缩短当前基于发现广播的 20 秒故障恢复时间。
2. 增加 AD/SSO、设备证书或 mTLS、项目 ACL、审计和磁盘配额。
3. 为现有局域网自动升级增加企业代码签名、集中审计和 IT 下发的防火墙规则。
4. 增加按 GPU 厂商的空闲策略、显存和温度保护。
