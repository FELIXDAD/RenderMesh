# GitHub / 开源本地渲染农场方案调研

> 调研日期：2026-08-18
>
> 范围：可部署在本机、局域网或自有机房的开源渲染调度方案，重点是 Blender 与通用 DCC。
>
> 来源口径：只采用项目官方仓库、官方文档与官方 API；活跃度取 GitHub/Gitea 的默认分支 `pushed_at`/最新提交和 latest release/tag。Star 仅作社区体量参考，不代表质量。
>
> 规模口径：下文的“适合规模”是根据架构复杂度、官方定位和部署依赖作出的选型建议，并非项目声明的硬上限。

## 结论先行

1. **本地 Blender、尤其是 Windows 局域网：优先 Flamenco。** 它由 Blender Studio 维护，Manager、Worker、Blender 插件和 Web UI 是一套完整产品，Windows/Linux/macOS 都有官方包。对 2～几十台节点最省改造成本。
2. **多 DCC、希望成熟且尽量自托管：优先比较 Afanasy 与 OpenCue。** Afanasy 的 DCC 插件覆盖更广、混合操作系统路径映射更直接；OpenCue 的架构、资源调度和可观测性更适合较大制作环境，但 PostgreSQL、Cuebot、RQD、共享存储等运维成本明显更高。
3. **想要现代化、单二进制、OpenJD：SQI 值得做技术验证，但不应直接押生产。** 截至本次调研稳定版仅 v0.2.0，HA/PostgreSQL/Kubernetes 仍在后续阶段，认证与任务隔离在 `main` 已合并但尚未正式发布。
4. **AWS Deadline Cloud Customer-Managed Fleets 不是完整本地方案。** 开源的 worker agent 可以跑在本地机器，但调度、身份、日志等控制面依赖 AWS Deadline Cloud、IAM 和 CloudWatch；它适合 AWS 混合云，不适合离线 LAN 或要求控制面完全自持的场景。
5. **不建议新项目采用 Deadline 10、BlendNet 或年代久远的 Blender 小项目。** Deadline 10 的 GitHub 仓库只开放脚本和组件而非完整调度器，且产品已进入维护模式；BlendNet 已归档。

## 快速对比

| 方案 | 活跃度（截至 2026-08-18） | 许可证 | 平台与 DCC | 部署模型 | 建议规模 | 判断 |
|---|---|---|---|---|---|---|
| **Flamenco** | 最新提交 2026-08-14；tag **v3.9.3**（2026-07-23） | GPL-3.0 | Win / Linux / macOS；Blender 优先，任务类型可扩展 | 1 个 Manager + 多 Worker + Blender 插件；NAS/共享目录或 Shaman | 2～几十台 Blender 节点 | **Blender 首选** |
| **OpenCue** | GitHub push 2026-08-12；release **v1.19.1**（2026-04-08）；约 952 stars | Apache-2.0 | 核心服务通常容器/Linux；RQD 可接 Linux/macOS/Windows；CueSubmit 内置 Blender/Maya/Nuke，任意 shell 命令可扩展 | PostgreSQL + Cuebot + RQD + 客户端/可选 REST、Web、监控 | 中大型、几十到数百节点 | **规模化首选，运维较重** |
| **CGRU / Afanasy** | GitHub push 2026-08-12；release **3.4.1**（2026-04-26）；约 302 stars | LGPL-3.0 | Win / Linux 有官方包，macOS 可源码构建；Blender、Maya、Houdini、Nuke、C4D、AE、3ds Max 等 | afserver + afrender + GUI/Web/CLI；可混合 OS 路径映射 | 小中型到工作室级 | **多 DCC 实用首选** |
| **Deadline Cloud CMF** | worker-agent push 2026-08-17；release **0.32.0**（2026-08-15）；约 17 stars | worker agent 为 Apache-2.0；AWS 服务非开源 | 官方 worker 配置覆盖 Linux/Windows；任何可命令行参数化的软件，使用 OpenJD | 本地/EC2 worker + **AWS 托管控制面** + IAM + CloudWatch；附件可走 S3 | 已使用 AWS 的混合云团队 | **不是完整本地自托管** |
| **SQI** | GitHub push 2026-08-17；release **v0.2.0**（2026-07-13）；约 33 stars | AGPL-3.0（另有商业许可） | Win / Linux / macOS；Blender、Maya、Houdini、Nuke submitter；OpenJD/任意命令 | 单 server + pull worker；mDNS；可选 Docker Compose | 试验、小型团队 | **有潜力，当前偏早期** |
| **Lifeblood** | GitHub push 2026-08-14；release **v0.14.0**（2026-07-12）；约 26 stars | GPL-3.0 | Blender、Houdini 与通用命令/工作流 | Scheduler + Worker + 节点图 Viewer | 个人/小团队实验 | **灵活，但仍是 0.x 小项目** |
| **Pholid** | GitHub push 2026-06-02；release **v1.0.1**（2026-04-05）；约 5 stars | 主仓 Apache-2.0；集成 Flamenco/GPL 组件，分发时需核对第三方许可 | Blender + NVIDIA CUDA/OptiX；Linux / Windows WSL2 | Docker Compose；Spring Boot + Flamenco + Prometheus/Grafana | 个人/小型 GPU 实验农场 | **可观察，不宜替代 Flamenco** |
| **LogicReinc.BlendFarm** | GitHub push 2025-02-02；release **v1.1.6**（2024-02-04）；约 530 stars | GPL-3.0 | Blender；宣称跨平台，CPU/GPU（CUDA/OpenCL/OptiX） | 独立客户端 + headless server；局域网自动发现 | 2～5 台个人设备 | **功能贴题但维护风险偏高** |

## 1. Flamenco：Blender 局域网的默认答案

官方源码现在以 Blender 自有 Gitea 为主，并非 GitHub 镜像；因此 GitHub 上搜到的 `blender/flamenco` 镜像或第三方 Docker 仓库都不应当作权威源。官方仓库仍活跃：默认分支最新提交为 2026-08-14，最新 tag 为 v3.9.3（2026-07-23）。[官方仓库](https://projects.blender.org/studio/flamenco)、[仓库 API](https://projects.blender.org/api/v1/repos/studio/flamenco)、[tags API](https://projects.blender.org/api/v1/repos/studio/flamenco/tags?limit=10)、[commits API](https://projects.blender.org/api/v1/repos/studio/flamenco/commits?limit=1)

官方 v3.9.3 下载同时提供 Windows、Linux、macOS Intel 和 Apple Silicon 包，每个包都包含 Manager 与 Worker；Blender 插件由 Manager Web 界面下载。[官方下载](https://flamenco.blender.org/download/)

### 架构与支持

- 一台机器运行 **Flamenco Manager**，各渲染机运行 **Flamenco Worker**；艺术家通过 Blender 插件提交，通过 Web UI 管理任务。
- 内置 Simple Blender Render（帧序列）和 Single Image Render（分块后合并），任务类型可用 JavaScript 扩展；官方文档另列出合成节点、Cycles CUDA/OptiX 等第三方任务示例。[内置任务类型](https://flamenco.blender.org/usage/job-types/builtin/)、[第三方任务](https://flamenco.blender.org/third-party-jobs/)
- 素材可直接放在所有节点均能访问的共享存储，也可在提交时复制到共享存储，或使用 Shaman 内容寻址存储。[共享存储](https://flamenco.blender.org/usage/shared-storage/)
- **Windows 注意事项：** 官方文档明确要求用盘符挂载共享目录，不支持把 `\\SERVER\share` UNC 路径直接作为 Flamenco 路径。混合系统时应配置双向变量/路径替换。[共享存储](https://flamenco.blender.org/usage/shared-storage/)、[跨平台变量](https://flamenco.blender.org/usage/variables/multi-platform/)

### 优点

- Blender 官方维护、版本适配最直接；安装包和 Blender 插件是一套交付物。
- 对小型局域网门槛低，不要求另建 PostgreSQL、消息队列或云账户。
- Manager/Worker/Web UI 职责清楚，任务队列、Worker 状态、优先级、标签等功能已覆盖常规动画帧农场。

### 局限与风险

- 产品方向是 **Blender-first**，不是 Maya/Houdini/Nuke 的统一工作室调度器。
- 共享存储、素材依赖打包、跨平台路径仍需规范化；“节点发现到了”不等于素材、字体、插件、缓存和 Blender 版本一致。
- 第三方 CUDA/OptiX job type 不是核心内置能力，升级前应做真实 `.blend`、贴图、缓存与输出回收测试。
- 官方说明 Linux 是主要开发平台，Windows/macOS 属于 best effort；权限与组织功能也刻意保持最小。它适合可信局域网的小团队，不应直接假设具备强多租户隔离。[项目定位](https://flamenco.blender.org/about/)

### 适合谁

个人工作室、学校实验室、2～几十台 Windows/Linux Blender 节点，以及希望最少运维即可获得队列和 Web 管理的团队。

## 2. OpenCue：成熟的大规模通用调度器

OpenCue 是 Academy Software Foundation 项目，源自 Sony Pictures Imageworks 的生产系统。官方定位是面向 VFX/动画的可扩展中央渲染农场，支持本地、云与混合部署，并明确提到生产环境可运行数百台 RQD 主机。[官方仓库](https://github.com/AcademySoftwareFoundation/OpenCue)、[官方文档](https://docs.opencue.io/)、[GitHub 仓库 API](https://api.github.com/repos/AcademySoftwareFoundation/OpenCue)、[最新 release API](https://api.github.com/repos/AcademySoftwareFoundation/OpenCue/releases/latest)

### 架构与支持

- **PostgreSQL** 保存调度状态；**Cuebot** 负责作业、帧和主机调度；每台渲染机运行 **RQD**；CueGUI/CueSubmit/CLI/Python API 负责提交与管理。可选 REST Gateway、OpenCueWeb、Prometheus/Grafana/Loki 等组件。[Sandbox 服务清单](https://docs.opencue.io/docs/developer-guide/sandbox-testing/)、[Cuebot 部署](https://docs.opencue.io/docs/getting-started/deploying-cuebot/)、[RQD 部署](https://docs.opencue.io/docs/getting-started/deploying-rqd/)
- CueSubmit 预配置 Blender、Maya、Nuke；也能把任何 shell 命令作为 layer 提交，所以实际可接多数命令行 DCC/渲染器，但节点必须自行安装相应软件和许可证。[提交作业](https://docs.opencue.io/docs/user-guides/submitting-jobs/)
- RQD 需要访问素材与日志文件系统；大型部署通常使用 NFS 等共享文件系统。官方混合测试文档展示了 Docker 中的 Cuebot/Linux RQD 与本机 Windows/macOS/Linux RQD 共存，但这不是“Windows 一键生产部署”的承诺。[RQD 部署](https://docs.opencue.io/docs/getting-started/deploying-rqd/)、[混合 RQD 测试](https://docs.opencue.io/docs/developer-guide/hybrid-rqd-setup/)

### 优点

- 调度模型成熟：作业/层/帧、资源标签、核心与内存预留、主机拆分、多设施、NIMBY、自动预订等能力适合大型制作。
- Apache-2.0，API 和自动化界面丰富；v1.19.1 仍持续维护。
- Docker Compose 已能拉起本地沙盒，便于先验证调度与 Blender 提交。

### 局限与风险

- 组件多、运维面大：数据库、Java/Cuebot、gRPC、RQD、共享存储、客户端与监控都要版本管理。
- “支持任意 shell 命令”不等于拥有每个 DCC 的成熟提交器；复杂 Houdini/Maya/Arnold/RenderMan 流水线通常仍需 TD 编写 Outline、环境注入和许可证策略。
- 对只有 2～10 台 Blender Windows 机器的局域网通常明显过重。

### 适合谁

已有 Linux/Docker/PostgreSQL 运维能力、需要几十到数百节点、资源/许可证精细调度、多团队或多站点的制作环境。

## 3. CGRU / Afanasy：覆盖广、相对轻的多 DCC 方案

CGRU 是一套 CG 工具包，Afanasy 是其中的渲染农场管理器。项目始于 2005 年，但截至本次调研仍有 2026 年提交和 3.4.1 release，不属于“旧而废弃”。[官方仓库](https://github.com/CGRU/cgru)、[官方文档](https://cgru.readthedocs.io/en/stable/)、[GitHub 仓库 API](https://api.github.com/repos/CGRU/cgru)、[最新 release API](https://api.github.com/repos/CGRU/cgru/releases/latest)

### 架构与支持

- 中央 `afserver` 接收作业并分配任务；每台节点运行 `afrender`；通过 AfWatch Qt GUI、Web GUI、CLI 或 JSON/Python API 管理。[Server](https://cgru.readthedocs.io/en/stable/afanasy/server.html)、[Render client](https://cgru.readthedocs.io/en/stable/afanasy/render.html)、[API](https://cgru.readthedocs.io/en/stable/afanasy/api.html)
- 官方提供 Windows ZIP 与多种 Linux 包；macOS 可从源码构建，但没有同等的官方预编译包。系统可提供混合 OS 路径映射。[下载](https://cgru.readthedocs.io/en/stable/downloads/downloads.html)、[安装](https://cgru.readthedocs.io/en/stable/installation/installation.html)
- 集成面很宽：3ds Max、After Effects、Blender、Cinema 4D、Clarisse、Fusion、Houdini、LightWave、Maya、Natron、Nuke、Softimage；AfStarter 还列出 VRay、Arnold、Redshift、Cycles、Mantra 等。[软件集成索引](https://cgru.readthedocs.io/en/stable/software/software.html)、[AfStarter 支持列表](https://cgru.readthedocs.io/en/stable/afstarter/afstarter.html)、[Blender 插件](https://cgru.readthedocs.io/en/stable/software/blender.html)
- 调度支持 pools/tickets、资源监控、优先级、依赖、多任务主机、多主机任务、许可证数量限制、NIMBY、Wake-on-LAN。[官方主页](https://cgru.info/)

### 优点

- 在开源方案中 DCC 插件覆盖最广，Windows 混合农场和路径翻译是明确功能。
- 既可通过现成 DCC 插件提交，也可用 JSON/Python API 或任意命令扩展。
- 相比 OpenCue，最小可用部署的外部基础设施更少。

### 局限与风险

- 文档风格和插件成熟度不一致；例如 Maya 页面明确标注“Documentation is outdated”。不能把列表里的每个 DCC 都视为已在最新版本完成回归。[Maya 文档](https://cgru.readthedocs.io/en/stable/software/maya.html)
- 初始配置、环境脚本、路径映射、service/parser 概念有学习成本，界面与配置体验不如新式产品统一。
- 对生产采用，应逐一验证目标 DCC 版本、渲染器插件、许可证服务器与退出码/进度解析。

### 适合谁

需要 Blender 之外还要调度 Houdini、Nuke、Maya、After Effects 等软件，希望完全本地自托管，又不想承担 OpenCue 全套基础设施的中小型工作室。

## 4. AWS Deadline Cloud Customer-Managed Fleets：本地算力，云端控制面

AWS 开源了 [Deadline Cloud worker agent](https://github.com/aws-deadline/deadline-cloud-worker-agent)（Apache-2.0）及各类 DCC submitter/样例，但 **Deadline Cloud 调度服务本身不是开源、自托管组件**。[worker-agent 仓库 API](https://api.github.com/repos/aws-deadline/deadline-cloud-worker-agent)、[最新 release API](https://api.github.com/repos/aws-deadline/deadline-cloud-worker-agent/releases/latest)、[官方架构说明](https://github.com/aws-deadline/deadline-cloud-worker-agent/blob/mainline/docs/dev/architecture.md)

### 它能做什么

- Customer-Managed Fleet 的 worker 可以位于 EC2、本地机房或托管数据中心；worker 生命周期与软件环境由用户管理。[Fleet 类型](https://docs.aws.amazon.com/deadline-cloud/latest/userguide/manage-fleets.html)
- Deadline Cloud 使用 OpenJD 描述任务，任何能通过命令行和参数控制的软件都可接入；官方仓库提供 Blender、Maya、Houdini、Nuke、Cinema 4D、Unreal 等 submitter 与样例。[工作原理](https://docs.aws.amazon.com/deadline-cloud/latest/userguide/how-it-works.html)、[AWS Deadline GitHub 组织](https://github.com/aws-deadline)
- 官方 worker host 文档覆盖 Linux 和 Windows。[Worker host 配置](https://docs.aws.amazon.com/deadline-cloud/latest/developerguide/worker-host.html)

### 为什么不是完整本地方案

- worker 始终向 `scheduling.deadline.<region>.amazonaws.com` 发起 HTTPS 连接，并把 agent/session 日志发往 CloudWatch Logs；使用 job attachments 时还会访问 S3。即使 worker 在局域网，也依赖 AWS Region、IAM 凭证和云端调度 API。[CMF 网络数据流](https://docs.aws.amazon.com/deadline-cloud/latest/developerguide/cmf-network.html)
- 可以用 PrivateLink + Direct Connect/VPN 避免公网，但这只是私网连接 AWS 服务，不是离线或本地控制面。
- 本地静态节点可选择 `NO_SCALING`；自动扩缩通常还需 EventBridge、Lambda/Auto Scaling 等 AWS 基础设施。[CMF 扩缩](https://docs.aws.amazon.com/deadline-cloud/latest/developerguide/create-auto-scaling.html)

### 适合谁

已经采用 AWS/IAM/CloudWatch，希望本地节点与云节点统一排队、需要 OpenJD 和云上扩缩的团队。若目标是“断网仍可用、控制面和日志完全留在 LAN”，应选 Flamenco、Afanasy、OpenCue 或继续自研。

## 5. 其他值得关注的项目

### SQI：最值得跟踪的新通用方案

[SQI](https://github.com/uberware/sqi) 由 Smedge 背后的 Uberware 开发，目标是从几台局域网工作站扩展到混合云。它使用 OpenJD，提供单二进制 server/worker、mDNS 自动发现、Web UI、路径翻译、S3 兼容存储、usage pool，以及 Blender/Maya/Houdini/Nuke 的应用内 submitter。[README](https://github.com/uberware/sqi#readme)、[文档](https://uberware.github.io/sqi/)、[仓库 API](https://api.github.com/repos/uberware/sqi)、[最新 release API](https://api.github.com/repos/uberware/sqi/releases/latest)

但截至 v0.2.0：

- 简单模式默认信任局域网、不启用认证；认证、RBAC、LDAP/OIDC 和任务 OS 用户隔离已在 `main` 合并，但尚未随稳定版发布。
- PostgreSQL、HA scheduler、NATS 直连 worker 和 Kubernetes manifests 属于 Phase 4，当前尚未实现。
- 项目新、社区和生产样本有限；AGPL-3.0 对修改后网络服务的源码提供义务需要法务确认。

**建议：** 做 3～5 节点真实素材 PoC，验证断点恢复、服务重启、Worker 离线、资产路径和多用户隔离后再考虑生产。

### Lifeblood：面向 CG 工作流的节点图调度器

[Lifeblood](https://github.com/permahorse/lifeblood) 由 Scheduler、Worker 和节点图 Viewer 组成，能按 CPU/GPU/自定义资源调度任务，并提供 Blender、Houdini submitter；官方定位更偏个人与小团队的通用 CG workflow automation，而不是传统大规模帧队列。[README](https://github.com/permahorse/lifeblood#readme)、[仓库 API](https://api.github.com/repos/permahorse/lifeblood)、[v0.14.0 release](https://github.com/permahorse/lifeblood/releases/tag/v0.14.0)

项目当前活跃，但仍是 v0.14.0、社区约 26 stars。节点图和动态资源模型很灵活，适合把 Houdini/Blender 前后处理串成工作流；对纯 Blender 帧渲染，它没有 Flamenco 的官方集成优势，对多 DCC 生产成熟度也还不能替代 Afanasy/OpenCue。**建议作为实验性工作流引擎验证。**

### Pholid：Flamenco + NVIDIA GPU 可观测性套件

[Pholid](https://github.com/M-Nikox/Pholid) 并非从零实现调度器，而是用 Docker Compose 组合 Spring Boot 控制面、Flamenco、容器化 Blender worker、Prometheus/Grafana、cAdvisor 与 NVIDIA GPU exporter，重点是 Linux/WSL2 上的 CUDA/OptiX。[README](https://github.com/M-Nikox/Pholid#readme)、[仓库 API](https://api.github.com/repos/M-Nikox/Pholid)、[最新 release API](https://api.github.com/repos/M-Nikox/Pholid/releases/latest)

当前 README 明确把 multi-user auth 与 HTTPS 放在 v2 分支；项目仅约 5 stars、历史较短。它适合作为 **Flamenco GPU 容器化与监控的参考实现**，不宜在缺少安全加固和长期维护评估时替代官方 Flamenco。

### LogicReinc.BlendFarm：贴近个人 LAN，但更新偏慢

[LogicReinc.BlendFarm](https://github.com/LogicReinc/LogicReinc.BlendFarm) 是独立 Blender 网络渲染器，提供局域网自动发现、headless server、Blender 版本自动下载、帧/图块拆分和 CPU/GPU 渲染。[README](https://github.com/LogicReinc/LogicReinc.BlendFarm#readme)、[仓库 API](https://api.github.com/repos/LogicReinc/LogicReinc.BlendFarm)、[最新 release API](https://api.github.com/repos/LogicReinc/LogicReinc.BlendFarm/releases/latest)

它的交互模型很适合 2～5 台个人设备，但最新 release 停在 2024-02、最后 push 为 2025-02；README 的已测试 Blender 版本止于 3.1，且明确警告不要把服务直接暴露到公网。README 仍提及 OpenCL 等旧 Blender GPU 路径，因此在 Blender 4.x/5.x、现代驱动与安全边界上必须重新验收。**建议只作实验或参考，不作为当前首选。**

### WeRender：概念很贴近，但仓库成熟度不足

[WeRender](https://github.com/toxzak-svg/werender) 的设计与小型 Blender LAN 农场非常贴近：README 宣称 mDNS 零配置发现、自动资产打包/分发、API key、Worker 断线恢复与失败帧重派、Web dashboard，以及 Blender 4+/Python 3.10+ 和 Cycles/Eevee 支持。[README](https://github.com/toxzak-svg/werender#readme)、[仓库 API](https://api.github.com/repos/toxzak-svg/werender)

但官方仓库证据与 README 的发布表述不一致：仓库创建于 2026-01-14、最后 push 为 2026-01-27，当前 0 stars/0 forks，GitHub Releases 为空；README 却标注 `v0.2.0-beta`，示例还指向一个不存在的 `releases/download/v0.2.0/demo-cube.blend`。这表示它更像早期原型，尚无可复现的正式发布链路。**可借鉴其资产分发、故障重派和 API key 设计，不建议作为生产依赖。**

### RenderChan：依赖感知前端，不是农场调度器

[RenderChan](https://github.com/morevnaproject-org/RenderChan) 会分析动画工程依赖，只重渲染发生变化的依赖与目标；任务可本机执行，也可交给 CGRU/Afanasy。仓库截至 2026-08-11 仍有 push，BSD-3-Clause。[README](https://github.com/morevnaproject-org/RenderChan#readme)、[仓库 API](https://api.github.com/repos/morevnaproject-org/RenderChan)

它适合与 Afanasy 组合减少无效重算，但自身不负责多节点调度，不应与 Flamenco/OpenCue/Afanasy 放在同一层替换。

## 6. 明确排除或不建议新部署

| 项目 | 原因 | 一手来源 |
|---|---|---|
| **AWS Thinkbox Deadline 10** | GitHub 仓库仅是“open-source scripts and components”，并非完整 Deadline 调度器源码。产品自 2025-11-07 起进入 maintenance mode，新客户不可用；现有客户只获得安全和关键修复，AWS 建议转 Deadline Cloud。 | [GitHub 仓库](https://github.com/ThinkboxSoftware/Deadline)、[官方维护模式 FAQ](https://docs.thinkboxsoftware.com/products/deadline/latest/1_User%20Manual/manual/maintenance-mode-faq.html)、[AWS 可用性公告](https://aws.amazon.com/about-aws/whats-new/2025/10/aws-service-availability/) |
| **BlendNet** | 仓库已归档，最后 push/release 均为 2022-03-02，不适合作为新基础设施。 | [GitHub 仓库](https://github.com/state-of-the-art/BlendNet)、[仓库 API](https://api.github.com/repos/state-of-the-art/BlendNet) |
| **cozybuild/docker-blender-render-cluster** | GitHub 已归档，最后 push 为 2020-09；只是旧 Docker 集群方案。 | [GitHub 仓库](https://github.com/cozybuild/docker-blender-render-cluster)、[仓库 API](https://api.github.com/repos/cozybuild/docker-blender-render-cluster) |
| **Brenda** | 最后 push 为 2015-02，目标是旧 AWS Blender 农场，不符合当前本地/局域网部署要求。 | [GitHub 仓库](https://github.com/jamesyonan/brenda)、[仓库 API](https://api.github.com/repos/jamesyonan/brenda) |

## 推荐决策路径

```text
主要是 Blender？
├─ 是
│  ├─ 要完全本地、跨 Windows/Linux、尽快可用 → Flamenco
│  ├─ 特别想要 Docker + NVIDIA 监控，能接受早期项目 → Pholid PoC
│  └─ 只需 2～5 台个人机器、愿意承担兼容性验证 → BlendFarm 实验
└─ 否，多 DCC
   ├─ 需要广泛现成 DCC 插件、混合 OS、较轻部署 → Afanasy
   ├─ 需要大型资源调度、多站点、成熟 API，能运维 DB/容器 → OpenCue
   ├─ 已深度使用 AWS，接受云控制面 → Deadline Cloud CMF
   └─ 想采用 OpenJD + 现代单二进制架构 → SQI PoC，暂不直接上生产
```

## 建议的 PoC 验收清单

无论选哪个项目，都不应以“端口能开、节点能上线”作为验收完成。至少验证：

1. **真实工程依赖：** 外部贴图、链接库、字体、缓存、Alembic/USD、插件和色彩配置均能在 Worker 找到。
2. **版本一致性：** Blender/DCC、渲染器、插件、GPU 驱动与许可证在调度端和 Worker 端可追踪且一致。
3. **失败恢复：** 杀掉 Worker、重启 Manager/server、断开 NAS、任务超时后不会重复覆盖或丢失结果。
4. **Windows 路径：** 盘符、UNC、大小写、跨 Linux 路径映射和输出目录权限通过实测。
5. **结果完整性：** 帧范围无缺帧/重帧，EXR 多层、元数据和临时文件原子回收正确。
6. **安全边界：** 未授权节点不能加入；作业命令、Web/API、日志和素材访问按用户或可信 LAN 边界隔离。
7. **可观测性：** 队列、重试、Worker 资源、失败日志、许可证耗尽和 GPU OOM 都能从界面或 API 定位。
8. **规模基准：** 用同一个 50～200 帧工程测单机与多机吞吐、启动/传输开销、NAS 峰值和调度器重启恢复时间。

## 最终建议

若当前目标是为本仓库的 **Windows + Blender + 局域网** 场景寻找可直接借鉴或替代的开源基础：

- 第一轮应部署 **Flamenco v3.9.3** 做 2～3 节点对照测试，重点对比节点发现、资产打包、共享存储、任务取消、重试和真实输出回收。
- 若未来明确要覆盖 Houdini/Nuke/Maya，再做 **Afanasy 3.4.1** PoC；只有在资源调度、几十/数百节点或多站点需求成立时，才值得投入 **OpenCue 1.19.1** 的基础设施成本。
- **Deadline Cloud CMF** 可作为混合云扩容路线，不应当作“离线本地农场”的候选。
- **SQI/Pholid** 适合持续跟踪和实验，不宜成为当前稳定版交付的唯一依赖。
