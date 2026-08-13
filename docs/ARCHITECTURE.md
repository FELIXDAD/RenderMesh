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

UDP 8390 广播协议版本、随机 nonce、节点 ID、节点自身 IPv4 地址列表、名称、HTTP 端口、共享状态、CPU/GPU 型号、CPU 策略、任务数和空闲时间。协议 v2 把名称、CPU 策略、空闲时间和硬件字段全部纳入 HMAC-SHA256 签名；升级过渡期同时发送字段与签名规则保持旧版不变的 v1 兼容信标，使尚未升级的节点仍能发现新版节点。新版接收端收到同节点的 v1 信标时会保留已由 v2 验证的名称、CPU 策略、空闲时间及硬件信息；只有始终未发送 v2 的旧节点仍沿用旧版较弱的字段保证。程序枚举所有已启用的 IPv4 网卡，根据前缀分别计算定向广播地址；`/31`、`/32` 等无广播网卡会跳过。发现签名始终使用内部随机集群凭证，不使用或广播六位邀请码。接收端要求 UDP 实际来源 IP 位于签名地址列表中，无效包不会覆盖已认证节点。广播不可用时，用户可保存一个手动 `IPv4:端口`；程序周期性请求 `/health` 并完成 `/v1/prove` 挑战，验证通过后进入同一个调度池。Origin 只向在线、验证有效、已开启共享且当前无任务的节点派发。远端接单后若节点离线，或重新报告为空闲却没有回传结果，Origin 会在 20 秒宽限期后将帧重新排队。

TCP 8391 提供 `/health`、`/v1/prove`、`/v1/task`、`/v1/result`、`/v2/pair`、`/v2/cache/status`、`/v2/cache` 和 `/v2/task`。六位邀请码只是人工输入的配对码：新节点进入待配对状态后，用户必须明确填写一台已加入集群的可信局域网 `IPv4:端口`；服务端按来源 IP 限制尝试次数，验证成功才把内部随机集群凭证交给新节点。邀请码不会出现在发现广播中。

Origin 对打包完成的 `.blend` 计算流式 SHA-256。协议 v2 先以哈希、Blender 版本、渲染引擎、GPU 要求和插件快照查询 Worker；只有缓存不存在或兼容条件变化时才上传工程，随后每帧任务只发送元数据。缓存上传以 1 MiB 块流式写入 `.receiving`，完成后原子替换并重新计算完整 SHA-256，哈希不符即删除拒绝。协议 v1 仍保留整文件逐帧上传，供升级过渡期的旧节点使用。

服务端单请求上限为 100 GiB，并在接收前检查剩余磁盘空间。上传使用可中止的数据提供器与 5 秒阻塞超时，退出不会被失联节点长期卡住。每个 Worker 原子地保留一个执行槽，缓存兼容性检查也占用该槽，避免并发请求绕过“单 Blender 进程”限制。每次派发都有新的随机 attempt token；只接受当前租约的结果，掉线重派后的旧 Worker 无法覆盖新结果。

## 持久化

默认数据目录：

```text
%LOCALAPPDATA%\RenderMesh\
  config.json
  jobs.json
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

## Blender 进程边界

Worker 通过参数数组启动 Blender，不经过 shell。正式渲染使用：

```text
blender --background --disable-autoexec source.blend
        --render-output frame_###### --render-frame 42
```

输出格式与是否附加扩展名由 `.blend` 工程设置决定，RenderMesh 不再传入 `--render-format` 覆盖工程。预检记录格式、扩展名和 `use_file_extension`；关闭自动扩展名的工程会按无扩展名文件接收。

只有别人发来的共享任务会被限制。本机用户自己提交并由本机兜底执行的任务不套用共享限额。主路径 Eevee / Cycles GPU 仅在空闲状态接单，检测到键鼠恢复后终止该次进程并向 Origin 请求重派；Cycles GPU 还要求 Worker 检测到已启用的非 CPU Cycles 设备。Cycles CPU 共享 Blender 在创建后先保持挂起，加入启用 `KILL_ON_JOB_CLOSE` 的 Windows Job Object，再恢复运行；用户活跃时设置 `HARD_CAP`，空闲到阈值后关闭 CPU rate control，使进程树可使用 100%。Windows 10/11 嵌套 Job 允许它在企业管理软件或受管宿主下正常工作。

## 生产化后续

1. 增加独立心跳与可续期租约，缩短当前基于发现广播的 20 秒故障恢复时间。
2. 增加 AD/SSO、设备证书或 mTLS、项目 ACL、审计和磁盘配额。
3. 增加签名安装包、自动升级和 IT 集中下发的防火墙规则。
4. 增加按 GPU 厂商的空闲策略、显存和温度保护。
