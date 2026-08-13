# 验证记录

验证日期：2026-08-13，Windows x64。

## 原生程序自检

`RenderMesh.exe --self-test --no-dialog` 返回 0，覆盖：

- 配置原子保存与重新读取，以及标准向量 HMAC-SHA256；
- 2 MiB 文件通过真实本机 HTTP 服务流式上传；
- Windows Job Object 在同一进程树上切换 15% 与 100%；
- 两个独立 NodeApp 实例通过不同 TCP 端口分发三帧任务并回传三个输出文件；
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

测试使用 Blender 官方便携构建 `5.2.0 LTS fbe6228777e7`；该大型测试工具不包含在发布 ZIP 中。

## 便携性

- Release EXE 使用 llvm-mingw 静态构建。
- 导入表只允许 Windows 系统 DLL。
- 发布 ZIP 在新的临时目录解压后重新运行完整自检和短时 headless 启动。
