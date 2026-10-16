# 命令行指南

适用于 RenderMesh 0.3.0。帮助文本使用简化技术英语。无参数启动桌面程序；有命令时执行操作并输出结果。`serve` 会持续运行节点。

## 启动方式

在 PowerShell 中切换到程序目录：

```powershell
.\RenderMesh.exe help | Out-Host
.\RenderMesh.exe help submit | Out-Host
.\RenderMesh.exe version --json | Out-Host
```

EXE 使用 Windows GUI 子系统。交互式 PowerShell 命令后接 `| Out-Host`，让控制台等待输出。脚本中可直接把输出交给 `ConvertFrom-Json`。在 cmd.exe 中使用 `start /wait RenderMesh.exe <command>`。

`help`、`version` 和 `inspect` 无需已运行的节点。任务及状态命令需要本机桌面程序或 `serve` 正在运行。`inspect` 仍需要可用的 Blender。

## 通用参数

| 参数 | 含义 |
| --- | --- |
| `--json` | 输出一行 JSON；`ok` 表示命令是否成功，失败时 `error` 给出原因 |
| `--data-dir <folder>` | 指定数据目录，默认 `%LOCALAPPDATA%\RenderMesh` |

节点和控制命令必须使用同一个数据目录。CLI 从该目录读取本机控制凭证，不需要把凭证写入命令。

## 完整流程

先启动桌面程序并配置 Blender。下面示例提交帧 1–120 的工程：

```powershell
$inspection = .\RenderMesh.exe inspect "D:\Projects\shot.blend" --json | ConvertFrom-Json
if (-not $inspection.ok) { throw $inspection.error }

$submission = .\RenderMesh.exe submit "D:\Projects\shot.blend" --name shot010 --wait --json | ConvertFrom-Json
if (-not $submission.ok) { throw $submission.error }
$jobId = $submission.job.id

.\RenderMesh.exe test $jobId --frames 1-120 --step 1 --package 5 --test-frames 1,60,120 --json | Out-Host
.\RenderMesh.exe job $jobId --json | Out-Host
```

`submit --wait` 等待工程同步，不等待渲染。`test` 发起测试后返回。重复查询 `job`，直到其 `job.stage` 为 `test_completed`。检查测试图后再运行：

```powershell
.\RenderMesh.exe start $jobId --json | Out-Host
.\RenderMesh.exe job $jobId --json | Out-Host
```

等待 `job.stage` 为 `completed`，再使用输出。命令返回 `ok: true` 只表示该次操作成功，不能用来判断全部帧已完成。

## 命令

| 命令 | 用途 |
| --- | --- |
| `help [command]` | 显示总帮助或单个命令的帮助 |
| `version` | 显示程序版本 |
| `status` | 显示本机和其他节点状态 |
| `jobs` | 列出任务，新任务在前 |
| `job <job-id>` | 显示单个任务及每帧状态 |
| `inspect <project>` | 检查工程及素材，不创建任务 |
| `submit <project>` | 检查、建立工程副本并创建任务 |
| `test <job-id>` | 设置帧范围并启动测试帧；用于 `ready` 状态 |
| `start <job-id>` | 启动完整范围；用于 `test_completed` 状态 |
| `stop <job-id>` | 停止后续帧，保留已完成结果 |
| `resume <job-id>` | 恢复已停止的帧 |
| `retry <job-id>` | 重试失败帧 |
| `delete <job-id>` | 删除任务和托管副本，保留源工程及工程输出目录中的结果 |
| `serve` | 无窗口运行节点，Ctrl+C 停止 |

### 检查和提交

```text
inspect <project> [--ignore-warnings]
submit <project> [--name <name>] [--ignore-warnings] [--wait]
```

- 默认任务名称为工程文件名。
- `--ignore-warnings` 接受预检警告。它不会补齐缺失文件。
- `--wait` 等待参与电脑的工程同步完成。
- `submit` 的成功 JSON 含 `job` 和 `report`，任务 ID 位于 `job.id`。
- `job` 的成功 JSON 含 `job`；`jobs` 的成功 JSON 含 `jobs` 数组。

### 测试参数

```text
test <job-id> [--frames <first>-<last>] [--step <n>]
             [--package <n>] [--test-frames <list>]
```

帧范围和步长默认使用工程设置。每包 1–20 帧，默认 5。测试帧默认取第一帧，也可使用 `1,50,100` 或 `1-3`。

### 无窗口节点

```powershell
.\RenderMesh.exe serve --data-dir "D:\RenderMeshNode" --port 8391 | Out-Host
```

在另一个 PowerShell 窗口使用：

```powershell
.\RenderMesh.exe status --data-dir "D:\RenderMeshNode" --json | Out-Host
```

同一数据目录同时只能有一个 RenderMesh 实例。`serve` 使用该目录的现有配置；共享默认关闭。需要接收远端任务时，先用桌面设置完成 Blender、匹配 ID 和共享配置，再退出桌面并运行 `serve`。

## 状态和退出码

主要流程为：

```text
uploading → ready → testing → test_completed → rendering → completed
```

同步、测试或渲染出错时，分别可进入 `upload_failed`、`test_failed`、`failed`。停止后进入 `stopped`。

帧状态包括 `pending`、`dispatching`、`queued`、`rendering`、`completed`、`failed` 和 `stopped`。

| 退出码 | 含义 |
| --- | --- |
| 0 | 命令执行成功 |
| 1 | 操作失败，读取错误信息 |
| 2 | 命令或参数错误 |
| 3 | 无法连接本机 RenderMesh 节点 |

本机控制接口只接受回环地址并检查本机凭证。不要把 `config.json` 放进脚本仓库。网络权限边界见 [安全说明](SECURITY.md)。
