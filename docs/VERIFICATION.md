# 验证记录

验证日期：2026-08-14，Windows x64。

## 原生程序自检

`RenderMesh.exe --self-test --no-dialog` 返回 0，覆盖：

- 配置原子保存与重新读取，以及标准向量 HMAC-SHA256；
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

1. 素材存在：预检通过，Blender 版本为 5.2，记录 11 个当前启用插件及可用版本，`missing=0`、`unpacked=0`，生成独立 packed `.blend`。
2. 删除原始 `checker.png` 后：RenderMesh 用 packed 副本成功生成 64×64 PNG，证明渲染不依赖原相对路径素材。
3. 对未打包原工程删除 `checker.png`：预检返回 3 并报告该 image 的绝对缺失路径，任务未创建。
4. Worker 环境与记录一致：兼容性检查返回 0。
5. 清单加入不存在的插件：兼容性检查返回 5，`missing_installed` 和 `missing_enabled` 都包含该插件。
   把已有插件版本改为 `999.0` 时同样返回 5，并在 `version_mismatch` 报告所需/实际版本。
6. VSE Image Strip 使用相对素材路径：删除原图后，预检通过 `bpy.utils.blend_paths` 将它报告为 `external` 缺失项并阻止提交。
7. 无摄像机工程旁预放一个旧 `frame_000099.png`：请求第 1 帧返回失败，不会把旧帧误认为新结果。
8. 图片序列缺少中间帧 `seq_0002.png`：预检精确报告缺失帧并返回 3；序列完整但 Blender 无法完整 pack 时报告全部 unpacked 路径并阻止分发。
9. Eevee、Cycles GPU 和 Cycles CPU 的远端任务都只在电脑空闲时接单；Cycles GPU 另外要求 Worker 检测到已启用的非 CPU Cycles 设备。
   Eevee 兼容性检查成功完成 1×1 真实渲染上下文探针并报告 `NVIDIA Corporation:NVIDIA GeForce RTX 5090 D v2`；本机 Cycles GPU 探测报告 `CUDA:NVIDIA GeForce RTX 5090 D v2` 和 `OPTIX:NVIDIA GeForce RTX 5090 D v2`，两种兼容性检查均返回 0。
10. 提交前在发起端对提交范围内每一帧执行低分辨率真实渲染，并检查材质、世界、灯光、合成器和节点组；测试工程返回 `validation_render=true` 和准确的 `validation_frames`。无相机或插件/节点/引擎执行异常时预检返回 3，不创建任务。
11. 用户于 2026-08-14 更新的 `D:\05_Documents\Desktop\测试.blend` 在 Blender 5.2 预检通过：Cycles GPU、PNG、11 个当前启用插件环境条目、`missing=0`、`unpacked=0`，主输出映射为 `D:\05_Documents\Desktop\tmp\####.png`，并检测到映射至 `tmp\AOV` 的 File Output 节点。本机真实提交 1–7 帧后只启动两个 Blender 进程（5+2），七张 PNG 和合成器 EXR 均发布到工程目录。
12. 另建 Blender 5.2 合成器工程，一次 Blender 启动同时生成主 PNG 和 File Output OpenEXR；清单准确记录 `primary` 与 `compositor/output-0/rgba_.exr`，并保留原节点输出目录。
13. 将上述真实 OpenEXR 交给桌面端预览转换路径，Blender 5.2 成功生成可由 WIC 显示的 PNG 缩略图。
14. GUI 运行时度量验证任务 ListView 的 `LVM_GETCOUNTPERPAGE=10`，主共享开关文字在 150% DPI 的可用区域内完整显示。
15. 双机实测发现测试 Worker 无法应用 Windows CPU Job 限速时，旧策略会以内部代码 91 终止 Blender 兼容检查。0.2.2 移除 CPU 百分比限速器和界面设置，兼容检查及所有远端渲染统一只在闲置状态运行；策略边界由内置自检覆盖。Blender 仍使用不含 CPU rate control 的 `KILL_ON_JOB_CLOSE` 生命周期 Job，受管环境无法加入时不会阻止渲染。

测试使用 Blender 官方便携构建 `5.2.0 LTS fbe6228777e7`；该大型测试工具不包含在发布 ZIP 中。

## 便携性

- Release EXE 使用 llvm-mingw 静态构建。
- 导入表只允许 Windows 系统 DLL。
- 发布 ZIP 在新的临时目录解压后重新运行完整自检和短时 headless 启动。
