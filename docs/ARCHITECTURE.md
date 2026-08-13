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

UDP 8390 广播协议版本、随机 nonce、节点 ID、节点自身 IPv4 地址列表、名称、HTTP 端口、共享状态、CPU 策略、任务数和空闲时间；整条关键发现信息使用集群码作 HMAC-SHA256 签名，不广播真实集群码。接收端要求 UDP 实际来源 IP 位于签名地址列表中，无效包不会覆盖已认证节点。Origin 派发前还会向 `/v1/prove` 发起随机挑战，验证该端点确实持有集群码，避免从另一台主机重放广播后取得工程和令牌。Origin 只向在线、签名有效、已开启共享且当前无任务的节点派发。远端接单后若节点离线，或重新广播为空闲却没有回传结果，Origin 会在 20 秒宽限期后将帧重新排队。

TCP 8391 提供 `/health`、`/v1/prove`、`/v1/task` 和 `/v1/result`。任务文件以 1 MiB 块流式读写到 `.receiving` 临时文件，完成后原子替换；服务端单请求上限为 100 GiB，并在接收前检查剩余磁盘空间。每个 Worker 原子地保留一个执行槽，避免并发请求绕过“单 Blender 进程”限制。每次派发都有新的随机 attempt token；只接受当前租约的结果，掉线重派后的旧 Worker 无法覆盖新结果。

## 持久化

默认数据目录：

```text
%LOCALAPPDATA%\RenderMesh\
  config.json
  jobs.json
  tools\preflight.py
  tools\compatibility.py
  jobs\<job-id>\source.blend
  jobs\<job-id>\outputs\
  incoming\<task-id>\source.blend
  incoming\<task-id>\outputs\
  failed-results\<task-id>.txt
```

程序重启时，把未完成的 Assigned/Dispatching/Rendering 帧恢复为 Pending；完成结果和错误记录保留。

## Blender 进程边界

Worker 通过参数数组启动 Blender，不经过 shell。正式渲染使用：

```text
blender --background --disable-autoexec source.blend
        --render-output frame_###### --render-format PNG --render-frame 42
```

只有别人发来的共享任务会被限制。本机用户自己提交并由本机兜底执行的任务不套用共享限额。Cycles CPU 共享 Blender 在创建后先保持挂起，加入启用 `KILL_ON_JOB_CLOSE` 的 Windows Job Object，再恢复运行；用户活跃时设置 `HARD_CAP`，空闲到阈值后关闭 CPU rate control，使进程树可使用 100%。GPU/未知引擎无法用通用 Windows CPU Job 百分比代表真实算力占用，所以仅在空闲状态接单，检测到键鼠恢复后终止该次进程并向 Origin 请求重派。Windows 10/11 嵌套 Job 允许它在企业管理软件或受管宿主下正常工作。

## 生产化后续

1. 用 SHA-256 内容缓存避免同一工程逐帧重复上传。
2. 增加独立心跳与可续期租约，缩短当前基于发现广播的 20 秒故障恢复时间。
3. 增加 AD/SSO、设备证书或 mTLS、项目 ACL、审计和磁盘配额。
4. 增加签名安装包、自动升级和 IT 集中下发的防火墙规则。
5. 增加按 GPU 厂商的空闲策略、显存和温度保护。
