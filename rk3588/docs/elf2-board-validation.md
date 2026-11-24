# ELF2 板端验证与 VM 删除门槛

`jammy-deploy` 的主机侧链接闭包通过，不代表板端运行通过。将程序和 `profiles/jammy-deploy/sysroot` 的运行时库部署到 RK3588 后，逐项保存命令、日志、版本和失败恢复证据。

## 验证清单

1. **V4L2/DMA-BUF 采集**：确认 `/dev/video*` 格式、帧率和 DMA-BUF fd；连续采集 5 分钟无 dequeue 超时、fd 泄漏或帧率下降。
2. **RKNN 推理**：记录 NPU runtime 版本、模型输入输出和每帧耗时；连续 1000 帧无 `rknn_query`/`rknn_run` 错误。
3. **RGA 转换**：记录 NV12/RGB 转换尺寸、stride 和 `imcheck` 结果；验证边界帧和异常尺寸不会崩溃。
4. **h264_rkmpp 编码与 RTMP**：确认编码器名为 `h264_rkmpp`，保存 `ffmpeg -encoders`/应用日志、码率、GOP、RTMP 推流和 VLC/播放器证据。
5. **AAC 编码**：确认 `avcodec_find_encoder_by_name("aac")` 成功，保存至少 30 秒 AAC 音频样本及播放/封装结果。
6. **MQTT 发布与重连**：发布实时消息，断网后恢复网络；保存连接、重连、离线缓存补发和消息序列证据。
7. **Modbus 采集**：保存串口参数、寄存器映射、原始帧和解析值；断开/恢复设备时无死锁。
8. **断网本地录像与恢复**：断网期间持续写本地片段，磁盘空间受控；网络恢复后上传/推流恢复且 SQLite/文件队列无重复丢失。

## VM 删除门槛

只有以下七项全部完成，才可以考虑删除 `ubuntu1`：

1. `rockdev-tools/manifest.sha256` 已复制到 VM 目录之外并可复核；
2. Desktop 镜像 MD5、Jammy base、依赖 overlay、FFmpeg 和验证 manifest 已备份；
3. `buildroot-check` 与 `jammy-deploy` 可在独立 WSL 环境按恢复手册重建；
4. `verify_elf2_sdk.sh` 和全部离线测试重复通过；
5. 上述八项板端运行证据完整，且无未解释的警告；
6. 已在另一块存储或远程位置保存源码提交、脚本、清单和恢复日志；
7. Cyber 明确确认不再需要 VM，并在删除前做最后一次恢复演练。

本项目脚本永远不会删除 VM、VDI 或 VirtualBox 目录；删除是独立的人工决策，未满足门槛时保持 `ubuntu1` 关机但存在。
