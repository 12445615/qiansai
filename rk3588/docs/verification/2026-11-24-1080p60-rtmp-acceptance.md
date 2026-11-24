# RK3588 1080p60 RTMP 验收记录（2026-11-24）

## 结论摘要

| 项目 | 结果 | 关键数据 |
|------|------|----------|
| 主机侧全量测试 | 通过 | `make test` 与 7 组 Recovery/契约测试全部 exit 0 |
| Jammy strict 构建 | 通过 | AArch64 ELF，FFmpeg ABI 60，`RKMPP_ENCODER=available` |
| 板端部署 | 通过 | 候选探针 rc=0，服务 active，MD5 已替换 |
| 1080p60 采集/编码/推流 | 通过 | capture/stream 59.8–60.0 fps，`stream_failures=0` |
| AI 三核 | 通过 | 120 次采样 `dropped=0 failed=0`，三 ctx `core_mask=0x1/0x2/0x4` |
| MQTT | 通过 | 连接成功，3 秒周期心跳 |
| 600 秒稳定性 | 通过 | 板端 RSS +164 KB/+204 KB，Qt 解码/显示 59.98 fps |
| 服务重启生命周期 | 通过 | PID 变化、MD5 不变、重启后 59.9 fps |
| STM32 / Modbus / 上传 | 未验证 | 硬件无响应、Modbus 被环境禁用、上传端点不可达 |
| 画面彩色完整性 | 部分验证 | 无黑行/黑列、无拉伸；现场为灰墙，无法验证色度 |

## 1. 基线与产物

- 分支：`rk3588-1080p60-rtmp`
- 基线：`d607021`（`codex/elf2-build-env`）
- 代码 HEAD（本文档提交前）：`d59776e`
- 变更规模：75 个文件，+6484 / -590
- 候选 ELF：`main`，836696 字节，MD5 `3550bf4acab75b1e4538fd4457ec7950`
- 构建配置：`PROFILE=jammy-deploy`，`FFMPEG_ABI=60`，`RKMPP_ENCODER=available`，Paho MQTT 与 libmodbus 均为 `enabled`
- 依赖闭包：`libavcodec.so.60`、`libpaho-mqtt3c.so.1`、`libmodbus.so.5`、`librockchip_mpp.so.1`、`librga.so.2`、`librknnrt.so` 全部解析到 Jammy sysroot

提交序列：

```
（本文档提交）docs(rk3588): record 1080p60 RTMP acceptance evidence
d59776e test(rk3588): default encoder probe to 1080p60
99475d4 chore(rk3588): ignore local self-test and bench binaries
bf3939e test(recovery): add include path to runner contract build
e62b40b fix(rk3588): repair double-encoded MQTT log strings
b4097f2 docs(rk3588): document recovery and 1080p60 deployment
6224f18 feat(storage): harden local video persistence and upload
97cb8eb feat(recovery): add proof and runtime lifecycle guarantees
df91a4e feat(rk3588): integrate Jammy 1080p60 pipeline
4c427b1 docs(rk3588): plan full 1080p60 integration
78b68f6 docs(rk3588): design full 1080p60 integration
```

## 2. 主机侧验证

在隔离 worktree 内执行，全部 exit 0：

| 验证项 | 命令 | 结果 |
|--------|------|------|
| 构建/配置测试 | `make test` | 通过 |
| 1080p60 源码契约 | `bash tests/test_1080p60_source_contract.sh` | 通过 |
| Recovery 构建契约 | `bash tests/test_recovery_build.sh` | 通过 |
| Recovery runner 契约 | `bash tests/test_recovery_runner.sh` | 通过 |
| Recovery 设计基线 | `bash tests/test_recovery_design_baseline.sh` | 通过 |
| compile-fail | `make recovery-compile-fail-test` | 通过 |
| ASan/UBSan | `make recovery-sanitize-test` | 通过 |
| 交叉边界与 digest | `make recovery-cross-check` | 通过 |
| clean strict build | `make clean && make PROFILE=jammy-deploy -j` | 通过 |
| 辅助自检程序 | `store-video-test`、`encoder-test`、`store-debug` | 通过 |

### 变更覆盖复核

源工作区 `rk3588/` 共 92 条 tracked/untracked 状态，其中源码、脚本、文档类 65 个文件与隔离 worktree 逐一比对：

- 54 个文件字节一致（直接导入）；
- 11 个文件存在差异，均为本次有意的修正或裁剪：
  `.gitignore`（新增本地自检产物）、`aliyun_mqtt.c`（乱码日志改英文）、
  `encoder_test.c`（默认 1080p60 并可传参）、`INTERVIEW.md` / `TECHNICAL_DOC.md`
  （改为 1080p60 事实、去掉个人绝对路径）、Recovery 设计基线与计划（重冻结哈希、
  路径占位）、`mk/recovery.mk`（`rg` 改 `grep` 以兼容无 ripgrep 环境）、
  `recovery/src/runtime.cc`（`SYS_pidfd_open` 回退定义）、
  `test_recovery_design_baseline.sh` 与 `test_recovery_digest_probe.sh`
  （新哈希与 Jammy multiarch 路径）；
- 无遗漏文件，无备份或构建产物进入索引。

构建存在 20 条既有警告（`av_init_packet` 弃用、若干未使用的静态函数），无 error，不阻断交付，未在本次改动中消除。

## 3. 板端部署

被替换版本：

- 内核：`5.10.209 #62 SMP Fri Nov 20 23:18:39 CST 2026`
- 系统：Ubuntu 22.04.5 LTS
- 原 `/root/main`：836648 字节，MD5 `46c499d30ae347314e6d45e1c0d9ee91`
- 原服务：active，capture/stream 59.9 fps，`stream_failures=0`

部署步骤与结果：

1. 候选物上传为 `/root/main.candidate-1080p60`，板上 MD5 与宿主一致。
2. `ldd` 无 `not found`；`media_probe` 返回 0（`h264_rkmpp` 与 `aac` 编码器均存在）。
3. 停止服务后以 `encoder_test rtmp://192.168.1.101:1935/live/test 1920 1080 60` 探针初始化：`streamer_init rc=0`，MPP 输出 `w:h=1920:1080 stride=1920:1088`、CBR 3 Mbps、60/1 fps、GOP 30。
4. 备份为 `/root/main.bak_20261124_101454-pre1080p60`，原子替换 `/root/main`，服务于 10:14:55 UTC 启动。
5. 启动后 `systemctl is-active` 为 active，`NRestarts=0`。

## 4. 功能回归

### 视频链

- 启动日志：`[Parent][Video] capture_fps=59.9 stream_fps=59.9 stream_failures=0`
- 编码子进程：`[Child][Encode] fps=59.9 avg_ms=3.1–3.8 max_ms=5–13`
- ffprobe：`codec_name=h264 width=1920 height=1080 r_frame_rate=60/1 avg_frame_rate=125/2`
- 平均帧率 62.5 是时间戳统计结果，解码端实测稳定 59.8–60.0 fps。

### AI

- `[AI] RKNN worker started`，三上下文 `core_mask=0x1/0x2/0x4`，输入 `640x640x3` dma-buf。
- 600 秒窗口 120 次统计，`dropped=0 failed=0`，`last_us` 约 69–87 ms，`submitted` 持续增长。

### MQTT

- `[Aliyun] connected; uploading and resending offline data`
- 每 3 秒发布心跳到 `/k29ovUMboAH/0122/user/update`。

### 本地存储

- `/var/lib/camera_flow` 因根分区 100% 满不可用，按设计回退到 `/userdata/camera_flow`（可用约 263 MB）。
- 20 秒分片正常创建：当前分片 6 秒内由 5.05 MB 增长到 7.58 MB。
- 600 秒内 30 次分片轮转；非事件连续分片在关闭时被 unlink，这是基线已有路径（`main.c` 对应条件未在本次变更中修改），DB 保留上传队列记录。
- 配置水位 `high=2.8 GB`、`low=2.5 GB`，而 `/userdata` 分区仅 982 MB；当前依靠分片轮转维持约 260 MB 空闲，水位阈值与分区容量不匹配。

### 上传

- 上传端点 `http://10.139.8.231:8080/api/video/upload` 在当前实验网络不可达。
- 600 秒内 17 次 `curl` 超时，上传器按 20 秒上限退避重试，进程无崩溃、无阻塞，离线 DB 持续记录。

### 服务重启

- `systemctl restart` 后 MainPID 由 10075 变为 12137，`/root/main` MD5 不变。
- 重新启动后 59.9 fps、`stream_failures=0`，无 fatal/segfault/IOMMU 错误。
- 重启后 Qt 60 秒播放：1920×1080，解码/显示 59.80 fps，58 个采样点。

## 5. 600 秒稳定性

板端采样 62 点、覆盖 616 秒：

| 指标 | 数值 |
|------|------|
| 父进程 RSS | 131200 → 131364 KB（+164 KB） |
| 视频子进程 RSS | 34664 → 34868 KB（+204 KB） |
| CPU 均值 | 父 13.0%，子 15.1% |
| 温度 | 79.5–85.9 °C |
| `/userdata` 可用 | 260548–268604 KB |
| 分片轮转 | 30 次 |
| capture/stream fps | 59.8–60.0 |
| `stream_failures=0` 行数 | 118 |
| `stream_failures>0` 行数 | 0 |
| IOMMU/CSI 错误 | 0 |
| fatal/segfault | 0 |

Qt 端 600 秒：

| 指标 | 数值 |
|------|------|
| 输入 | 1920×1080 |
| 解码 fps | 59.980 |
| 显示 fps | 59.980 |
| 采样点 | 599 |
| RSS | 96976 KiB（首尾无增长，脚本阈值内） |
| CPU | 17.5% |

## 6. 画面完整性

从 RTMP 实时流抓取一帧（1920×1080 PNG）：

- 平均 RGB：113.7 / 114.0 / 113.7
- 标准差：58.8 / 59.5 / 59.2
- 全黑行：0；全黑列：0
- 唯一颜色数：8662
- 画面内容为灰色墙面与线槽，无明显拉伸、死行、周期性错位或全黑区域。

由于现场镜头对准灰墙，画面本身缺少彩色内容，无法据此验证色度还原；需要用彩色标板补测。

## 7. 未通过 / 未验证项

1. **STM32 安全联锁**：`/dev/ttyS9` 以 115200 打开成功，但串口统计 `rx:0`，心跳始终 `stm32_online=0`，说明 MCU 未响应。部署前旧版本同样为 `stm32_online=0`，不是本次构建引入。
2. **Modbus 传感器**：服务以 `SENSOR_MODBUS_DISABLE=1` 启动，采集器按配置跳过，未验证 Modbus 读写。
3. **音频告警**：`[Audio Alert] IPC Pipe initialized` 初始化成功，但无告警事件触发，未验证实际播报。
4. **上传成功路径**：端点不可达，只验证了失败重试与离线缓存，未验证成功上传与服务端确认。
5. **画面色度**：现场为灰墙，无法验证彩色还原。
6. **根分区写满**：`/` 仍有 199 MB 空闲，`/var/lib/camera_flow` 不可写，存储已按设计回退到 `/userdata`；根分区容量问题未在本次范围内处理。
7. **基线硬编码 MQTT 设备密钥**：`aliyun_mqtt.c` 中 `PASSWORD` 为明文常量，`d607021` 基线已存在，本次未引入也未改动；建议改为构建期注入并轮换密钥。
8. **上传鉴权占位符**：`video_uploader.h` 的 `VIDEO_UPLOADER_HTTP_AUTH_TOKEN` 仍为 `CHANGE_ME_TOKEN`，上传端点的真实鉴权配置未验证。

## 8. 回退方法

```bash
ssh root@192.168.1.105
cp -a /root/main.bak_20261124_101454-pre1080p60 /root/main
systemctl restart rk3588-main.service
journalctl -u rk3588-main.service -f
```

回退后应恢复原 MD5 `46c499d30ae347314e6d45e1c0d9ee91`，并确认 capture/stream 回到 59.9 fps。

## 9. 证据索引

本仓库 `docs/verification/evidence/`：

- `board-600s-summary.txt`：板端 600 秒日志统计
- `board-600s-resources.tsv`：板端 62 点资源采样
- `qt-600s-summary.txt`：Qt 600 秒验收结论
- `qt-restart-60s-summary.txt`：重启后 60 秒验收结论
- `ffprobe-after-deploy.txt`：部署后码流参数

worktree 本地完整证据（未提交）：`.planning/rk3588-1080p60-rtmp/evidence/`，包含
`final-verify.log`、`board-600s-journal.txt`（4241 行）、`qt-600s.log`、`frame-after-deploy.png`。
