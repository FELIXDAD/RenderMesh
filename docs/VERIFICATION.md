# 验证记录

验证日期：2026-08-14，Windows x64。

## 原生程序自检

`RenderMesh.exe --self-test --no-dialog` 返回 0，覆盖：

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

测试使用 Blender 官方便携构建 `5.2.0 LTS fbe6228777e7`；该大型测试工具不包含在发布 ZIP 中。

## 便携性

- Release EXE 使用 llvm-mingw 静态构建。
- 导入表只允许 Windows 系统 DLL。
- 发布 ZIP 在新的临时目录解压后重新运行完整自检和短时 headless 启动。
