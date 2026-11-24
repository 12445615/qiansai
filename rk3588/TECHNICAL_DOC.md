# RK3588 安全监控系统技术文档

## 目录

1. [系统拓扑](#1-系统拓扑)
2. [技术栈](#2-技术栈)
3. [视频采集层](#3-视频采集层)
4. [进程架构与 IPC](#4-进程架构与-ipc)
5. [AI 推理链路](#5-ai-推理链路)
6. [视频编码与推流](#6-视频编码与推流)
7. [OSD 叠加绘制](#7-osd-叠加绘制)
8. [安全融合决策](#8-安全融合决策)
9. [传感器接入](#9-传感器接入)
10. [端云协同与离线容灾](#10-端云协同与离线容灾)
11. [问题排查实录](#11-问题排查实录)

---

## 1. 系统拓扑

```
┌────────────────────────────────────────────────────────────┐
│                      硬件                                   │
│  OV13855 ─MIPI─> RK3588 ISP ─DMA─> /dev/video11 (NV12)    │
│  STM32   ─UART─> /dev/ttyS9                                │
│  传感器   ─RS485─> /dev/ttyUSB0                             │
└────────────────────────────────────────────────────────────┘
                           │
          ┌────────────────┼────────────────┐
          ▼                ▼                ▼
     main (父进程)    main (子进程)     后台线程
     ├─ 采集          ├─ 编码            ├─ MQTT 上报
     ├─ AI 推理       ├─ OSD 画框        ├─ 视频补传
     ├─ 安全融合      ├─ RTMP 推流       ├─ 传感器采集
     ├─ 电子围栏      └─ 本地 TS 录制     └─ STM32 通信
     └─ 语音报警
```

---

## 2. 技术栈

| 层级 | 技术 | 用途 | 选型原因 |
|------|------|------|----------|
| 视频采集 | V4L2 MPLANE + DMA-BUF | 从 ISP 取 NV12 帧 | mmap 零拷贝；DMA-BUF 可跨进程传递，GPU/NPU/MPP 直接读 |
| 2D 加速 | RGA IM2D API | 颜色转换、缩放、数据搬运 | RK3588 硬加速，替代 CPU memcpy；`wrapbuffer_fd()` 直接接受 DMA fd |
| AI 推理 | RKNN Toolkit / librknnrt | YOLOv8 NPU 前向推理 | RK3588 NPU 6 TOPS，`rknn_create_mem` 返回 DMA-BUF，与 RGA 零拷贝对接 |
| 视频编码 | FFmpeg 6 + h264_rkmpp | MPP 硬件 H.264 编码 | 面向 1920×1080@60fps，避免 CPU 软编码成为瓶颈 |
| 协议库 | libpaho-mqtt3c、libmodbus、libcurl、libsqlite3 | MQTT / Modbus / HTTP / 本地库 | C 库生态成熟，交叉编译支持好 |
| 字体渲染 | FreeType | OSD 字模预渲染 | 渲染一次，复用贴图，避免每帧调用 FreeType |
| 交叉编译 | GCC 10.3 aarch64-none-linux-gnu | 在 x86 主机编译 ARM64 二进制 | Rockchip BSP SDK 预置工具链，版本匹配 |
| 构建 | GNU Make | 单 Makefile | 项目规模不需要 CMake；显式控制链接顺序解决符号依赖 |

---

## 3. 视频采集层

### 3.1 设备节点

`/dev/video11` 是 RK3588 ISP 的输出 capture 节点。上游链路：

```
OV13855 sensor → MIPI D-PHY → VICAP → ISP(3A+降噪+缩放) → /dev/video11
```

ISP 的 AE/AWB/AF 运行在 M0/RISC-V 协处理器固件中，业务代码不需干预。但 ISP AE 在光照条件变化大时可能产生"呼吸效应"（亮度周期性振荡），通过 V4L2 control 可手动锁定曝光值来验证和缓解。

### 3.2 初始化流程

- `open("/dev/video11")`
- `VIDIOC_S_FMT` 设置 NV12 MPLANE 1280×720
- `VIDIOC_S_PARM` 请求 60fps，并以驱动回报的实际帧间隔决定编码/推流帧率
- `VIDIOC_REQBUFS` 申请 4 个 mmap buffer
- `mmap()` 映射到用户空间
- `VIDIOC_EXPBUF` 将每个 buffer 导出为 DMA-BUF fd（给 AI 和编码用）
- `VIDIOC_QBUF` 全部入队
- `VIDIOC_STREAMON` 开始采集

### 3.3 主循环

```
VIDIOC_DQBUF 取帧
  ├─ send_fd(dma_fd) → 子进程编码推流
  ├─ ai_pipeline_process_frame(dma_fd) → NPU 推理
  ├─ 安全融合决策
  ├─ MQTT / STM32 / 语音报警
  └─ VIDIOC_QBUF 还帧
```

### 3.4 为什么用 DMA-BUF 而非 memcpy

NV12 1920×1080@60fps 约为 186.6MB/s。逐帧 CPU memcpy 会显著占用 A55 与内存带宽；DMA-BUF 让 RGA、NPU、MPP 共享同一组底层缓冲，减少 CPU 数据搬运。

---

## 4. 进程架构与 IPC

### 4.1 为什么父子进程而非多线程

两个硬性原因：

**原因一：MPP 编码器 teardown 崩溃。** `h264_rkmpp` 的 `avcodec_free_context()` 在销毁编码器时，MPP 内核驱动引用计数存在 bug，触发 kernel Oops 导致整板死机。验证过程：在单进程中调用 `streamer_clean()` 反复确认崩溃可复现；`fork()` 后让子进程直接 `_exit()`，内核自动回收资源绕过 buggy 路径，问题消失。

```c
// main.c:2115-2120
/*
 * Do not call streamer_clean() here. avcodec_free_context(h264_rkmpp)
 * has repeatedly triggered MPP ref-count errors followed by kernel Oops
 * on this board. The child process is short-lived, so let process exit
 * reclaim userspace resources and avoid the buggy MPP teardown path.
 */
```

**原因二：故障隔离。** RTMP 网络中断时编码失败不应影响采集和 AI 检测。子进程可随时安全退出，父进程按指数退避重启。

### 4.2 进程间传递 DMA-BUF

`socketpair(AF_UNIX, SOCK_STREAM)` 创建父子进程通信通道。父进程通过 `sendmsg(SCM_RIGHTS)` 把 DMA-BUF fd 发给子进程，内核在子进程侧克隆一个指向同一物理内存的 fd，零数据拷贝。

子进程收到帧后编码推流，完成后发 ACK 'k' 告知父进程，然后 `close(fd)` 释放本地引用。

### 4.3 AI 检测结果共享

`mmap(MAP_SHARED | MAP_ANONYMOUS)` 创建父进程和推理线程之间的共享内存区域 `DetectSharedState`。`fork()` 后子进程自动继承该映射，因此子进程能直接读 AI 结果用于 OSD 画框。

SeqLock 无锁同步：`version` 字段的 LSB 作为写锁标记——写者写前递增（变奇数），写完递增（变偶数）；读者循环读取 version，保证前后一致且为偶数时才取快照。选择原因：推理线程持有锁 <1μs，mutex 上下文切换开销（~3μs）反而更大。

---

## 5. AI 推理链路

### 5.1 模型与标签

YOLOv8 6 类：`helmet`, `no-helmet`, `no-vest`, `person`, `vest`, `fire`。使用 FP16 模型（`best_fp16.rknn`），输入 640×640 RGB。

### 5.2 推理架构

NPU 内存池（4 slot）+ 任务队列（2 job）：

```
父进程:                           推理线程:
  acquire_input_buffer()              pop_job() 阻塞等待
  获取空闲 slot                       │
  │                                  process_job()
  ├─ RGA improvess()                   ├─ DMA_BUF_IOCTL_SYNC
  │   NV12 DMA → NPU DMA RGB            ├─ rknn_inputs_set()
  │   色域转换 + 缩放 + letterbox        ├─ rknn_run()
  │                                     ├─ post_process()
  └─ submit_input_buffer()              │   NCHW 解码 + 坐标反算
      入队 job, signal 推理线程          │   + 排序 + 类内 NMS
                                        └─ write_detect_results()
                                            写入 DetectSharedState (SeqLock)
```

### 5.3 为什么每 3 帧推理一次

NPU 单次推理约 15-30ms，帧间隔 33ms。每帧都推理时 NPU 负载 100%，多出的 job 被 drop。3 帧推理一次（10Hz）既保证检测实时性，又降低 NPU 占用和功耗。安全融合决策本身仍逐帧执行（30Hz），AI 结果通过 1s Stale 超时和上一次快照补齐。

### 5.4 后处理关键细节

**NCHW 解码**：模型输出 [1,9,8400]，8400 个锚点，每点 9 通道（cx,cy,w,h + 5 类分数）。遍历提取最大类别概率，超阈值（0.5）后读 bbox 坐标。

**坐标反算（letterbox 逆运算）**：模型输入是 640×640 letterbox，需反算回 1280×720 原图：

```
scale = min(640/1280, 640/720) = 0.5
offset_y = (640 - 720×0.5)/2 = 140
原图 x = 模型 x / 0.5
原图 y = (模型 y - 140) / 0.5
```

**类内 NMS**：按置信度降序排列后，同类且 IoU > 0.45 的重复框被抑制。不同类不做抑制（如 fire 和 person 允许重叠）。

### 5.5 量化模型 candidates=0 问题

**现象**：`[AI] stats candidates=0 boxes=0`，`best_raw=0.000000`。

**排查过程**：
1. RGA 画框通路独立验证通过（固定中心测试框能正常显示），排除画框问题
2. `AI_CONF` 降到 0.01 仍然无框，排除阈值问题
3. 加 debug 日志看模型输出分布：bbox 坐标值正常（0~640），但类别分数全为零
4. 查模型量化参数：`output type=INT8 zp=-128 scale=2.526`。YOLOv8 的 [1,9,8400] 输出把 bbox（0~640）和 class score（0~1）混在一个 tensor 里量化，scale 被 bbox 大范围拉到大值，0~1 的类别分数量化后全部归零

**解决**：重转 FP16 非量化模型。后续若有性能需求，导出时将 bbox 和 class 分两个输出头分别量化。

---

## 6. 视频编码与推流

### 6.1 编码参数

| 参数 | 值 | 原因 |
|------|-----|------|
| 编码器 | `h264_rkmpp` | RK3588 MPP 硬编码，满足 1080p60 实时链路预算 |
| 码率 | CBR 3Mbps | 2Mbps 时复杂画面有块效应，3Mbps 是平衡点 |
| GOP | 15（0.5s） | 缩短 I 帧间隔降低首帧延迟和断流恢复时间 |
| Profile | baseline | 无 B 帧，编码延迟最低 |
| qmin/qmax | 18/38 | qmax=45 时场景切换码率失控，38 是实测上限 |
| rc_buffer | 200KB | 小缓冲 = 低延迟，但不过小导致质量波动 |
| tune | zerolatency | 关闭编码器内部帧重排序 |

### 6.2 推流流程

```
子进程:
  recv_fd(sock) 收到 DMA-BUF fd
  │
  ├─ RGA improcess: DMA→CPU 虚拟地址 (唯一一次数据搬运)
  │
  ├─ OSD 画框画字 (直写 Y 平面)
  │
  ├─ avcodec_send_frame(h264_rkmpp)
  │
  └─ av_write_frame → RTMP (或本地 mpegts 文件)
  │
  └─ send ACK → 父进程
```

### 6.3 RTMP 断网容错

```
第1级：子进程内切换 RTMP → 本地 .ts
第2级：子进程退出，父进程指数退避重启
第3级：本地 .ts 在 SQLite 登记，后台 HTTP 补传
```

退避节奏：1s → 2s → 4s → 8s → 16s → 30s（维持），一旦成功重置回 1s。

---

## 7. OSD 叠加绘制

### 7.1 为什么不走 RGA Alpha Blend

首次实现用 RGA RGBA→NV12 alpha blend 贴图。测试发现小字（14-20px FreeType 渲染）在 VLC/RTMP 播放端有**明显彩色花边**。

**根因分析**：NV12 的 UV 色度平面做 4:2:0 子采样（每 2×2 像素共用一个 UV 对）。小字的 RGBA 边缘像素在 alpha blend 时产生的色度分量，被子采样扩散到相邻像素。再经过 H.264 色度子采样量化，最终播放端表现为彩色噪点。

**解决方案**：OSD 标签和文字**只写 Y 亮度平面**，UV 保持 128（中性灰=无色）。

```
黑色底条: Y=16  UV=128
白色文字: Y=235 UV=128（背景128不写 = 底层黑色条的UV=128不产生色度分量）
检测框:   Bresenham 画线直写Y平面
类别色条: RGA imfill_t 写 6px 彩色矩形（色条宽不会产生彩边问题）
```

### 7.2 实现方式

- **画框**：`draw_nv12_line()` Bresenham 算法直接在 Y 平面画 4px 线条
- **黑底标签条**：`nv12_fill_rect()` memset 填充 Y 和 UV
- **文字**：`nv12_draw_stamp_luma()` 按 FreeType 预渲染的 RGBA `GlyphStamp` 遍历像素，alpha>0 时写 Y=235，不碰 UV
- **UV 寻址**：`yuv_frame->data[1] = data[0] + linesize[0] * 768`，NV12 的 UV 平面从 Y 平面后 768 行起始（独立编码帧缓冲区，不是 DMA-BUF）

---

## 8. 安全融合决策

### 8.1 输入维度

| 来源 | 数据 | 频率 |
|------|------|------|
| AI 视觉 | 6 类目标检测框 + 置信度 | 10Hz（有效），30Hz（补齐） |
| STM32 串口 | 烟感、可燃气、温度、急停、故障码、执行器状态 | 事件驱动 |
| Modbus | 气体浓度 (ppm)、温度 (°C)、湿度 (%RH) | 2Hz |
| 电子围栏 | 工作区/危险区坐标（颜色分割识别） | 锁定后静态 |

### 8.2 决策优先级

9 级级联判定，匹配即返回，不再下探：

```
1. STM32 手动复位           → 清除所有锁定，恢复通电
2. STM32 急停（硬件按钮）     → 严重/锁定/全部断电
3. STM32 故障码              → 严重/锁定/全部断电
4. STM32 等待复位状态         → 严重/等待
5. AI 火情 + 传感器确认       → 严重/断电关风机/语音报警
6. AI 入侵（多人进入工作区）    → 危险/断电/语音报警
7. AI PPE 违规（无帽/无背心）  → 警告/断电/语音报警
8. STM32 离线（30s 无数据）   → 警告/待机
9. 一切正常                  → 安全/通电
```

### 8.3 防抖策略

- **火警双因子确认**：AI 单独看到火不报警。必须 8 秒窗口内 STM32 传感器同步异常（烟感上升趋势或绝对值超阈值）才锁死火警。消除电焊火花、反光衣反光等单源误触发。
- **PPE 违规延迟确认**：5 秒首次出现延迟，人员瞬时经过不触发。
- **复位后火警忽略**：`SAFETY_RESET_FIRE_IGNORE_MS=5000`，手动复位后 5 秒内新火警被抑制。
- **AI 结果 Stale**：`SAFETY_AI_STALE_MS=1000`，超过 1 秒的 AI 结果不参与融合决策。

---

## 9. 传感器接入

### 9.1 Modbus RTU (RS485)

`sensor_modbus.c` 轮询读取寄存器：
- 气体浓度 (ppm)：寄存器 0
- 温度 (°C)：寄存器 1，`×0.1`
- 湿度 (%RH)：寄存器 2，`×0.1`
- 运行状态：寄存器 3
- 报警状态：寄存器 4

采样间隔 500ms（`SENSOR_READ_WAIT_MS=500`）。数据写入全局 `g_sensor_data`（mutex 保护），MQTT 线程定时读取上报。

### 9.2 STM32 串口通信

`safety_interlock_client.c` 通过 `/dev/ttyS9`、115200 8N1 与 STM32 通信。

帧格式：`A5 5A | VER | TYPE | SEQ | LEN | PAYLOAD | CRC16 | 0D`

RK3588 每 500ms 发送 `AI_STATUS` 和 `FUSION_DECISION` 帧。接收 STM32 上报的：
- `SENSOR_DATA`：烟感、可燃气体、温度
- `EVENT`：急停、故障、复位等待、复位完成
- `ACTUATOR_STATUS`：继电器、风扇、报警状态

30 秒无数据视为 STM32 离线（`SAFETY_STM32_OFFLINE_MS=30000`）。

---

## 10. 端云协同与离线容灾

### 10.1 MQTT 上报

`timerfd` 每 3 秒触发一次上报周期（`MQTT_REPORT_INTERVAL_SEC=3`），`poll()` 同时监听 timerfd、eventfd（外部立即上报信号）和 MQTT socket。

上报内容：传感器快照、AI 检测状态、STM32 设备状态、安全融合决策、区域识别结果。

### 10.2 离线缓存

断网时 MQTT payload 写入 SQLite `offline_queue` 表：
- `PRAGMA journal_mode=WAL`（写性能优于 DELETE 模式）
- `PRAGMA synchronous=NORMAL`（平衡安全性和速度）
- 上限 `100000` 条，超出按最旧 ID 删除

恢复联网后补发逻辑：
1. `local_store_fetch_batch()` 按自增 id ASC 取最旧 10 条
2. 逐条 `publish_payload()`
3. 成功 → `local_store_delete()` 删除已补发记录
4. 失败 → 停止当前批次，下次周期重试

### 10.3 视频本地分片

`video_store.c` 管理本地 mpegts 分片：
- 默认 20s/段
- 在 `video_segments` 表中索引每段
- 高水位 2.8GB / 低水位 2.5GB
- 超出高水位后从最旧记录开始删文件 + 删 SQLite 记录，直到低于低水位
- `video_uploader` 后台线程扫描 `state='pending'` 分片，HTTP multipart/form-data 上传后标记为 `uploaded`

### 10.4 时间源分离

两个时间源严格分工：
- `CLOCK_MONOTONIC`：退避间隔、轮转周期、RTMP 恢复重试判断
- `CLOCK_REALTIME`：文件名时间戳、SQLite 记录时间戳、MQTT 上报时间

原因：`CLOCK_MONOTONIC` 不受系统时间调整影响，`CLOCK_REALTIME` 产生人类可读的文件名和业务时间。混用会导致文件名出现 `1970-01-01`。

---

## 11. 问题排查实录

### 11.1 MPP 编码器 teardown 崩溃

| 项目 | 内容 |
|------|------|
| 现象 | 调用 `avcodec_free_context()` 后 `dmesg` 出现 `Unable to handle kernel paging request`，系统死机 |
| 定位方法 | 在 `main.c` 退出路径上逐行加 `sleep(3)` + `printf`，确定致命语句是 `streamer_clean()`；注释掉该调用后不再崩溃 |
| 根因 | MPP 驱动在 `h264_rkmpp` 的 close/free 路径上存在引用计数的 use-after-free，触发内核 page fault |
| 解决方案 | 父进程 `fork()` 子进程后，编码器生命周期绑定子进程。子进程 exit 时内核回收所有 fd 和内存，不触发 MPP free 路径 |

### 11.2 RGA 画框报错 `yuv not align to 2`

| 项目 | 内容 |
|------|------|
| 现象 | `RGA draw box failed: Invalid parameters: dst, Error yuv not align to 2` |
| 定位方法 | 搜索 RGA IM2D 文档，确认 NV12 上 `imfill_t` 的 x/y/w/h 必须偶数对齐 |
| 根因 | 检测框坐标可能是奇数，不满足 NV12 UV 交错的 2 对齐要求 |
| 解决方案 | 画框前 `x&~1`、`y&~1`、`w&~1|2`、`h&~1|2` 强制偶对齐；同时发现 `imfill_t` 在奇坐标上报错后改用 CPU Bresenham 画线替代，彻底绕开对齐限制 |

### 11.3 量化模型无检测输出

| 项目 | 内容 |
|------|------|
| 现象 | `[AI] candidates=0 boxes=0` 持续出现 |
| 排查链路 | ① RGA→编码画框独立验证通过 → ② `AI_CONF` 降到 0.01 无效 → ③ 加 debug 打印 `best_raw=0.000000` → ④ 检查模型量化参数 |
| 根因 | YOLOv8 输出 bbox（0~640）和 class score（0~1）混在同一 tensor 做 INT8 量化，bbox 大范围把 scale 拉到大值，class score 量化后全为零 |
| 解决 | 改用 FP16 非量化模型 |

### 11.4 OSD 小字彩边

| 项目 | 内容 |
|------|------|
| 现象 | RTMP/VLC 播放时，14-20px 大小的标签文字周围有红蓝彩色噪点 |
| 排查方法 | 分别检查原始 Y 平面（正常）、UV 平面（有色度分量泄漏）、H.264 编码后（噪点被放大） |
| 根因 | RGA RGBA→NV12 alpha blend 在小字边缘产生色度分量，经 YUV420 4:2:0 子采样扩散后，H.264 编码的色度量化进一步劣化 |
| 解决 | OSD 全部直写 Y 平面（Bresenham 画线 + FreeType 字模贴图），UV 保持 128 |

### 11.5 OV13855 呼吸效应（亮度周期性明暗摆动）

| 项目 | 内容 |
|------|------|
| 现象 | 画面每隔 1-3 秒变亮再变暗，全程自动曝光模式 |
| 排查链路 | ① `v4l2-ctl --list-ctrls` 列 sensor subdev 控制项 → ② `v4l2-ctl --set-ctrl=exposure_auto=1` 关 AE → 呼吸消失（确认 AE 振荡） → ③ `v4l2-ctl --set-ctrl=power_line_frequency=1` 设 50Hz 防频闪 |
| 根因 | ISP AE 算法在光照变化场景下欠阻尼振荡；未启用 50Hz 防频闪 |
| 解决 | 在 `camera_init()` 后通过 V4L2 control 固定曝光/gain 或设置 AE 目标值和步长，并通过 `power_line_frequency=1` 强制防频闪。代码中已有 `camera_sensor_lock_fps_controls()` 通过环境变量在 sensor subdev 上每 5s 重写 vblank/exposure/gain |

### 11.6 时间戳错乱（1970-01-01 路径）

| 项目 | 内容 |
|------|------|
| 现象 | 本地 `.ts` 文件落入 `video/1970-01-01/` 目录 |
| 排查方法 | `printf` 打印 `video_store_build_segment_path()` 中用的时间源 |
| 根因 | 调用 `clock_gettime(CLOCK_MONOTONIC, ...)` 获取的时间戳（从系统启动算起）被当成 `CLOCK_REALTIME` 使用，`localtime()` 转换后得到 1970 年 |
| 解决 | 分离两种时间源：退避/轮转/重试用 `CLOCK_MONOTONIC`；文件名/SQLite 时间戳/MQTT 上报用 `CLOCK_REALTIME`。`main.c` 中所有时间函数都加了 `mono_now_ms()` vs `wall_now_ms()` 命名区分 |

### 11.7 SQLite 离线数据关联失败

| 项目 | 内容 |
|------|------|
| 现象 | 日志打印"已保存一条离线记录"，但 `sqlite3` 查询为 0 |
| 排查方法 | 检查程序日志中 `SQLite 已就绪:` 后的实际路径 → 发现与查询的路径不一致 |
| 根因 | 环境变量 `CAMERA_FLOW_STORE_DIR` 未设置时回退到默认路径，与预期路径不同 |
| 解决 | 以日志打印的实际路径为准查询；同时在代码注释中明确记录了默认路径 |
