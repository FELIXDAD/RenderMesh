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

UDP 8390 广播协议版本、节点 ID、名称、HTTP 端口、共享状态、CPU 策略、任务数、空闲时间和集群码 SHA-256 短指纹，不广播真实集群码。Origin 只向在线、指纹一致、已开启共享且当前无任务的节点派发。远端接单后若节点离线，或重新广播为空闲却没有回传结果，Origin 会在 20 秒宽限期后将帧重新排队。

TCP 8391 提供 `/health`、`/v1/task` 和 `/v1/result`。任务文件以 1 MiB 块流式读写到 `.receiving` 临时文件，完成后原子替换；服务端单请求上限为 100 GiB，并在接收前检查剩余磁盘空间。每个 Worker 原子地保留一个执行槽，避免并发请求绕过“单 Blender 进程”限制。

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
```

程序重启时，把未完成的 Assigned/Dispatching/Rendering 帧恢复为 Pending；完成结果和错误记录保留。

## Blender 进程边界

Worker 通过参数数组启动 Blender，不经过 shell。正式渲染使用：

```text
blender --background --disable-autoexec source.blend
        --render-output frame_###### --render-format PNG --render-frame 42
```

只有别人发来的共享任务会被限速。本机用户自己提交并由本机兜底执行的任务不套用共享限额。共享 Blender 在创建后先保持挂起，加入启用 `KILL_ON_JOB_CLOSE` 的 Windows Job Object，再恢复运行。用户活跃时设置 `HARD_CAP`；空闲到阈值后关闭 CPU rate control，使进程树可使用 100%。Windows 10/11 嵌套 Job 允许它在企业管理软件或受管宿主下正常工作。

## 生产化后续

1. 用 SHA-256 内容缓存避免同一工程逐帧重复上传。
2. 增加任务租约、心跳、超时重派和幂等结果回传。
3. 增加 AD/SSO、设备证书或 mTLS、项目 ACL、审计和磁盘配额。
4. 增加签名安装包、自动升级和 IT 集中下发的防火墙规则。
5. 增加按 GPU 厂商的空闲策略、显存和温度保护。
