# 验证记录

验证日期：2026-08-13，Windows x64。

## 原生程序自检

`RenderMesh.exe --self-test --no-dialog` 返回 0，覆盖：

- 配置原子保存与重新读取，以及标准向量 HMAC-SHA256；
- `/23` 等 IPv4 前缀的定向广播地址计算，且每张网卡保留独立源地址；
- 2 MiB 文件通过真实本机 HTTP 服务流式上传；
- Windows Job Object 在同一进程树上切换 15% 与 100%；
- 手动 `IPv4:端口` 先经过 `/health` 与 HMAC 挑战验证并持久化，再由两个独立 NodeApp 实例通过不同 TCP 端口将七帧分为 5+2 任务包，经工程缓存回传主输出与合成器 EXR；
- 停止远端五帧包时，取消信号与派发竞态均被栅栏拦截；若 Worker 已开始，只允许当前帧收尾，后续帧不再启动；
- Worker 任务包贡献历史会记录分配的五帧、已完成的五帧与开始/结束时间，重建 NodeApp 后仍可读取；
- `RMBNDL1` 多文件包的安全路径、`../` 穿越拒绝、取消时临时文件清理和输出盘发布回滚；
- 错误类型的节点 JSON 会被拒绝而不会终止后台线程；旧版 Worker 不报告任务 ID 时，`assigned=1` 的长帧不会被误判为掉线，恢复空闲后仍可触发重派；
- 模拟 Worker 接单后离线，Origin 在租约宽限期后自动把该帧恢复 Pending，并由本机完成；
- 旧 Worker 使用已经失效的 attempt token 迟到回传时，Origin 返回 409，不会覆盖重派结果。

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
9. Eevee 工程被识别为 GPU 主路径，远端 Worker 在电脑活跃时拒绝接单；Cycles GPU 同样只在空闲状态运行，并要求 Worker 检测到已启用的非 CPU Cycles 设备。
   本机 Cycles GPU 探测报告 `CUDA:NVIDIA GeForce RTX 5090 D v2` 和 `OPTIX:NVIDIA GeForce RTX 5090 D v2`，兼容性检查返回 0。
10. 提交前在发起端对提交范围内每一帧执行低分辨率真实渲染，并检查材质、世界、灯光、合成器和节点组；测试工程返回 `validation_render=true` 和准确的 `validation_frames`。无相机或插件/节点/引擎执行异常时预检返回 3，不创建任务。
11. 用户提供的 `D:\05_Documents\Desktop\测试.blend` 在 Blender 5.2 便携版预检通过：Eevee、PNG、11 个当前启用插件环境条目、`missing=0`、`unpacked=0`，第 1 帧工程主输出映射为 `D:\05_Documents\Desktop\tmp\0001.png`。该工程没有 File Output 节点。
12. 另建 Blender 5.2 合成器工程，一次 Blender 启动同时生成主 PNG 和 File Output OpenEXR；清单准确记录 `primary` 与 `compositor/output-0/rgba_.exr`，并保留原节点输出目录。
13. 将上述真实 OpenEXR 交给桌面端预览转换路径，Blender 5.2 成功生成可由 WIC 显示的 PNG 缩略图。
14. GUI 运行时度量验证任务 ListView 的 `LVM_GETCOUNTPERPAGE=10`，主共享开关文字在 150% DPI 的可用区域内完整显示。

测试使用 Blender 官方便携构建 `5.2.0 LTS fbe6228777e7`；该大型测试工具不包含在发布 ZIP 中。

## 便携性

- Release EXE 使用 llvm-mingw 静态构建。
- 导入表只允许 Windows 系统 DLL。
- 发布 ZIP 在新的临时目录解压后重新运行完整自检和短时 headless 启动。
