# RK3588 安全监控系统 — 面试问答

## 项目概览

基于 RK3588 的工业安全监控系统，实现摄像头（OV13855）视频采集 → AI 目标检测（YOLOv8/6类）→ 安全融合决策 → 编码推流 + 本地存储 + 语音报警 + MQTT 上报 + STM32 联锁的全链路闭环。

---

## 一、架构设计

### Q1: 为什么选择父子进程架构而不是单进程多线程？

**答**：核心原因有两点：

**1. 编码器 teardown 会导致内核崩溃。** 具体来说，`h264_rkmpp` 是 RK3588 的 MPP 硬件编码器，每次调用 `avcodec_free_context()` 销毁编码器时，MPP 驱动内部的引用计数管理有 bug，会触发 kernel Oops 导致整板死机。这个问题是在实际调试中反复验证确认的——只要子进程退出（进程自然结束），内核会回收该进程的所有资源而不走 MPP 的 buggy teardown 路径。因此编码器必须跑在独立进程中，崩溃重启通过进程退出实现，不调用 `avcodec_free_context`。

```c
// main.c:2115-2122
/*
 * Do not call streamer_clean() here. avcodec_free_context(h264_rkmpp)
 * has repeatedly triggered MPP ref-count errors followed by kernel Oops
 * on this board. The child process is short-lived, so let process exit
 * reclaim userspace resources and avoid the buggy MPP teardown path.
 */
```

**2. 隔离推流故障域。** RTMP 推流依赖网络，网络中断时编码器会因为 `av_write_frame` 失败而报错。如果编码器和采集在同一个进程，推流失败可能导致整个采集链路挂掉。父子进程隔离后，子进程负责编码推流，挂了不影响父进程继续采集传感器和 AI 推理。父进程通过**指数退避策略**在 1s→2s→4s→...→30s 的间隔内自动重启子进程。

```
父进程 (采集+AI+传感器+MQTT) —— 永远不退出
    │
    └── 子进程 (编码+推流) —— 可以挂，挂了父进程重启它
```

---

### Q2: 父子进程之间怎么传视频帧？为什么不用共享内存？

**答**：用 Unix Domain Socket 的 `SCM_RIGHTS` 机制传递 DMA-BUF 文件描述符。

**为什么选 DMA-BUF + SCM_RIGHTS 而不是共享内存**：

| 方案 | CPU拷贝次数 | 内存带宽占用 | 延迟 |
|------|-----------|-------------|------|
| 共享内存 memcpy | 1次（NV12 1280×720 = 1.38MB/帧） | DDR 读写各 1.38MB | ~2-3ms |
| DMA-BUF SCM_RIGHTS | 0次 | 0 | <0.1ms |

NV12 1920×1080@60fps 需要搬约 186.6MB/s。用共享内存的话，每帧都要 CPU 做约 3.11MB 的 memcpy，既占用 A55 算力，也放大内存带宽和缓存压力。

DMA-BUF 的原理：V4L2 驱动分配的物理连续内存直接绑在 DMA-BUF 上，`sendmsg(SCM_RIGHTS)` 把文件描述符发给子进程时，内核在子进程中克隆一个新的 fd，指向**同一块物理内存**。整个过程没有任何数据拷贝。

```c
// ipc.c — 发送端
send_fd(sock, dma_fd);  // 传的是 fd 编号，不是数据

// 接收端
int child_fd = recv_fd(sock);  // 拿到同一个 DMA 内存区的 fd 克隆
```

**跟 Android 的 Binder 机制比**：Binder 底层也能传 fd 但走的 `binder_transaction` → `binder_translate_fd` 路径，本质类似，但 Unix Socket SCM_RIGHTS 是 POSIX 标准，嵌入式更轻量。

---

### Q3: 为什么采集用 mmap 而不是 userptr 或 DMABUF 直接取帧？

**答**：实际用了 mmap + VIDIOC_EXPBUF 的组合——mmap 映射到用户空间方便调试（可以直接拿虚拟地址看像素），但数据传输用 VIDIOC_EXPBUF 导出的 DMA-BUF fd。

```c
// camera.c:86 — mmap 映射
ctx->buffers[i].start = mmap(NULL, length, PROT_READ|PROT_WRITE, MAP_SHARED, fd, offset);

// main.c:2294 — 导出 DMA-BUF fd
ioctl(cam.fd, VIDIOC_EXPBUF, &expbuf);
dma_fds[i] = expbuf.fd;
```

这个设计的考量是：
- **mmap 映射**：CPU 可以直接读写当前帧（zone_detector 的逐像素颜色扫描需要 CPU 访问 NV12 Y 平面），同时也可以把同一 buffer 导出为 DMA-BUF 给 RGA/NPU/MPP 用
- **DMA-BUF 导出**：让 RGA、NPU、MPP 编码器都能零拷贝访问同一块物理内存

---

### Q4: AI 推理为什么也在父进程而不是子进程？

**答**：AI 推理结果需要同时被父进程（安全融合决策）和子进程（画面 OSD 画框）消费，通过共享内存 `DetectSharedState` 传递。

如果 AI 在子进程跑：
- 子进程挂了 → AI 停了 → 安全融合决策（火灾检测/PPE违规/入侵检测）全部失效 → 不合规
- 子进程重启需要重新加载 RKNN 模型 → 额外 2-3 秒冷启动时间

AI 在父进程跑：
- 父进程永远在线 → AI 永远在线
- 子进程随时重启不影响安全检测
- 父进程有 AI 结果做融合决策（主功能），子进程用同一结果画 OSD（辅助功能）
- 父子各用各的 DMA-BUF 引用，互不影响

**并行设计**：每一帧父进程同时做两件事——`send_fd()` 给子进程 + `ai_pipeline_process_frame()` 送 AI 推理，两者并行，AI 推理的 15-30ms 不阻塞推流。

```
父进程一帧循环:
  DQBUF 取帧
    ├─ send_fd(sock, dma_fd) ─────────► 子进程编码推流
    └─ ai_pipeline_process_frame() ───► NPU 推理 → 共享内存
  QBUF 还帧
```

---

## 二、零拷贝与硬件加速

### Q5: 整个链路中有几处拷贝？怎么做到近乎零拷贝的？

**答**：5 个数据搬运点，全部用硬件加速，**CPU memcpy 次数为 0**。

| 阶段 | 操作 | 硬件 | 是否零拷贝 |
|------|------|------|-----------|
| V4L2→父进程 | VIDIOC_DQBUF | DMA 引擎 | ✅ 零拷贝 |
| 父→子进程 | SCM_RIGHTS 传 fd | 无数据搬运 | ✅ 零拷贝 |
| NV12→NPU RGB | RGA `improcess` | RGA 硬件 blitter | ✅ 零拷贝（DMA→DMA） |
| NPU 输入→推理 | `rknn_inputs_set` | NPU DMA | ✅ 零拷贝 |
| DMA→编码帧 | RGA `improcess` | RGA 硬件 blitter | ❌ 一次必要的拷贝 |

唯一的一次硬件拷贝发生在 `streamer_push_zerocopy_overlay()` 中：

```c
// encoder.c:929-942 — 唯一的数据搬运，但走 RGA 硬件
src = wrapbuffer_fd(dma_fd, 1280, 720, RK_FORMAT_YCbCr_420_SP);  // DMA-BUF
dst = wrapbuffer_virtualaddr(yuv_frame->data[0], ...);             // CPU虚拟地址
improcess(src, dst, ...);  // RGA 硬件搬数据，CPU 不参与
```

**为什么这一次拷贝省不掉？** 因为要在 NV12 帧上画 OSD 检测框和文字——这需要 CPU 直接写 Y/UV 平面。而 DMA-BUF 映射的物理内存在 V4L2 驱动管理下，CPU 直接写 DMA buffer 有 cache 一致性问题（DMA 和 CPU 看到的数据可能不一致，需要 `DMA_BUF_IOCTL_SYNC` 做 cache flush，开销更大）。所以宁可让 RGA 硬件做一次 DMA→虚拟地址的拷贝，然后 CPU 安全地在虚拟地址上画框，最后交给 MPP 编码器。

**为什么不直接用 RGA 画框？** 试过，但 RGA 的 `imfill_t` 要求 x/y/w/h 必须 2 对齐（NV12 UV 交错的约束），实际检测框坐标经常是奇数，会导致 RGA 报错 `Error yuv not align to 2`。所以改用 Bresenham 算法直接写 Y 平面画线，绕开对齐限制。

---

### Q6: RGA 在链路中用了多少次？分别做什么？

**答**：RGA 用了 **3 次**，每次职责不同：

**第1次：AI 预处理** — `ai_preprocess_dma_to_rgb_fd()` (main.c:402)
```
输入: DMA-BUF fd → NV12 1280×720
输出: NPU DMA fd → RGB888 640×640
操作: 颜色空间转换 (YUV→RGB) + 等比缩放 (letterbox) + 黑边填充
```
一次 RGA `improcess` 调用完成三步，硬件操作。letterbox 计算公式：
```c
// scale = min(640/1280, 640/720) = 0.5
// 缩放后 640×360, 上下各 140px 黑边
```

**第2次：编码帧拷贝** — `streamer_push_zerocopy_overlay()` (encoder.c:929)
```
输入: DMA-BUF fd → NV12 1280×720
输出: CPU 虚拟地址 → NV12 1280×720
操作: 单纯的 1:1 数据搬运
```

**第3次：OSD 色条** — encoder.c:1011
```c
imfill_t(dst, (im_rect){bg_x, bg_y, 6, bg_h}, osd_class_color_rgb(id), IM_SYNC);
```
在标签左侧画 6px 宽的类别色条（RGB 颜色通过 RGA 硬件做到 NV12 上）。

**RGA 选型考量**：RK3588 有两套 2D 加速接口：老的 RGA API (`c_RkRgaInit` 等) 和新的 IM2D API (`improcess`/`imfill_t`/`wrapbuffer_fd`)。当前全部用 IM2D，因为：
- `wrapbuffer_fd()` 直接接受 DMA-BUF fd，不需要中间转换
- `IM_SYNC` 模式保证同步完成，不需要手动等待
- 新 API 文档更完善（Rockchip 官方推荐）

---

### Q7: NPU 推理的内存是怎么管理的？

**答**：用了一个 **4-slot 内存池 + 2-job 队列** 的异步推理架构。

```
┌─────────────────────────────────────────────┐
│  NPU 内存池 (MAX_SLOTS=4)                    │
│  slot_mems[0..3]: rknn_tensor_mem*          │
│  slot_in_use[0..3]: 使用标记                 │
│                                              │
│  父进程:                                     │
│    acquire_input_buffer() → 拿到空闲 slot    │
│    RGA 预处理 → NPU 内存                      │
│    submit_input_buffer() → job 入队          │
│                                              │
│  推理线程:                                   │
│    pop_job() → 取 job                        │
│    process_job():                            │
│      DMA_BUF_IOCTL_SYNC (cache 同步)         │
│      rknn_inputs_set() → NPU 读 DMA-BUF      │
│      rknn_run() → NPU 推理                   │
│      post_process() → YOLO 解码 + NMS         │
│      write_detect_results() → 共享内存        │
│      release_slot() → 归还 slot               │
└─────────────────────────────────────────────┘
```

**为什么是 4 slot + 2 job**：
- 4 slot：V4L2 也是 4 个 DMA buffer，一一对应，RGA 预处理可以流水线化
- 2 job：限制推理队列深度——NPU 推理耗时 15-30ms，帧间隔 33ms，超过 2 帧的积压意味着 NPU 跟不上采集速度，应该丢帧而不是无限排队

**NPU 内存直接用 DMA-BUF fd 的好处**：
```c
// rknn_worker.cc:312 — NPU 申请的内存本身就是 DMA-BUF
slot_mems[s] = rknn_create_mem(ctx, input_attrs[0].size);
```
NPU 内存在物理上是连续的，`rknn_create_mem` 返回的结构体里有 `fd` 和 `virt_addr`——fd 给 RGA 硬件做 DMA 写入（`wrapbuffer_fd`），virt_addr 给 CPU 做 FP16 转换。GPU 风格的统一内存模型。

---

## 三、YOLO 检测后处理

### Q8: YOLOv8 输出的 [1,9,8400] 是怎么解码成检测框的？

**答**：后处理在 [postprocess.cc](postprocess.cc) 中完成，分为 4 步：

**Step 1 — 确定张量形状**

NPU 输出是一个 [1, 9, 8400] 的 tensor（NCHW 布局）：
- dim[0]=1：batch
- dim[1]=9：通道（cx, cy, w, h + 5个类别分数）
- dim[2]=8400：锚点数量（YOLOv8 的 3 个检测头 concat 后 = 80×80 + 40×40 + 20×20 = 8400）

```c
// postprocess.cc:75-91 — 自适应推导形状
if (out_attr[0].n_dims >= 3 &&
    out_attr[0].dims[1] >= 5 &&
    out_attr[0].dims[1] <= 4 + OBJ_CLASS_NUM &&
    out_attr[0].dims[2] > 0) {
    channel_count = out_attr[0].dims[1];   // 9
    num_classes = channel_count - 4;        // 5
    num_anchors = out_attr[0].dims[2];     // 8400
}
```

**Step 2 — NCHW 遍历提取最高分类别**

```c
// postprocess.cc:130-148
for (int i = 0; i < 8400; i++) {
    for (int c = 0; c < 5; c++) {
        // NCHW 布局: class 分数在 (4+c)*8400 + i 位置
        float prob = sigmoid(read_output_value(output, (4+c)*8400 + i, total_elems));
        if (prob > max_class_prob) { ... }
    }
}
```

**Step 3 — 坐标反算（模型坐标 → 原始 1280×720 坐标）**

模型输入是 640×640 letterbox，检测框坐标在模型空间。需要反算回原图：

```c
// postprocess.cc:119-122 — letterbox 参数
float scale = min(640.0f/1280, 640.0f/720);   // = 0.5
float offset_x = (640 - 1280*0.5) / 2;          // = 0 (宽度正好)
float offset_y = (640 - 720*0.5) / 2;            // = 140 (上下黑边)

// postprocess.cc:171-174 — 坐标反算
res.box.left   = (int)((x1 - offset_x) / scale);  // 模型x÷0.5 → 原图
res.box.top    = (int)((y1 - offset_y) / scale);  // (模型y-140)÷0.5 → 原图
res.box.right  = (int)((x2 - offset_x) / scale);
res.box.bottom = (int)((y2 - offset_y) / scale);
```

**Step 4 — NMS 非极大值抑制**

```c
// postprocess.cc:192-208
按置信度降序排序 → 遍历: 对同类别且 IOU > 0.45 的后续框标记为 suppressed
// 只做类内 NMS，不同类别的框不做抑制（fire 和 person 可以重叠）
```

**为什么后处理跑在 CPU 而不是 NPU？** NPU 推理输出已经是 float，后处理的浮点运算量很小（8400×5×2 次 sigmoid + 几十个框的 IOU），CPU 完全可以胜任。把后处理放 NPU 反而增加 NPU 的调度开销。

---

### Q9: 量化模型导致的 candidates=0 问题是怎么回事？

**答**：这是一个实际踩过的坑。

现象：
```
[Parent][AI] stats ... candidates=0 boxes=0
[RKNN][debug] ... best_raw=0.000000 ...
```

根因分析：YOLOv8 的最终输出 [1,9,8400] 中，前 4 个通道是 bbox 坐标（范围 0~640），后 5 个通道是类别分数（范围 0~1）。INT8 量化时，qnt 参数 `scale=2.526` 由 bbox 坐标的大范围决定，导致 0~1 的类别分数被量化成了 `round(0.5/2.526) = 0`，信息完全丢失。

解决方案：
1. **当前方案**：使用 FP16 非量化模型（`best_fp16.rknn`），避免量化精度损失
2. **未来更优方案**：导出时把 bbox 和 class 分成两个输出头分别量化，各自使用合适的 scale

```c
// rknn_worker.cc:76-83 — FP16 模型需要做 uint8→float16 转换
static void convert_rgb_u8_to_fp16_inplace(void *buffer, uint32_t rgb_size) {
    uint8_t *rgb = (uint8_t *)buffer;
    rknpu2::float16 *fp16 = (rknpu2::float16 *)buffer;
    for (int64_t i = (int64_t)rgb_size - 1; i >= 0; --i) {
        fp16[i] = (float)rgb[i] / 255.0f;     // [0,255] → [0.0, 1.0]
    }
    // 从后往前转换，避免覆盖未处理的数据 (float16 是 2 字节, uint8 是 1 字节)
}
```

---

## 四、安全融合决策

### Q10: 安全决策的融合逻辑是什么？

**答**：安全决策融合了 **AI 视觉检测（YOLO）** + **STM32 传感器（烟感/温感/急停/故障）** + **电子围栏（工作区/危险区）** 三个维度。

决策优先级（从高到低，一旦匹配就不再下探）：

```
1. STM32 RESET_OK (手动复位)         → 清除所有锁定，允许通电
2. STM32 EMERGENCY_STOP (急停)       → 严重/锁定/断电
3. STM32 FAULT (故障)                → 严重/锁定/断电
4. STM32 RESET_WAIT (等待复位)        → 严重/等待
5. AI FIRE + SENSOR CONFIRM (火警+烟感/温感确认) → 严重/断电关风机
6. AI INTRUSION (入侵)               → 危险/断电报警
7. AI PPE VIOLATION (安全帽/反光衣违规) → 警告/断电
8. STM32 OFFLINE (串口断连)          → 警告/待机
9. AI ALL CLEAR (一切正常)            → 安全/通电
```

**关键设计决策**：

**1. 火灾需要传感器确认（双因子验证）**

```c
// main.c:1178-1192
if (ai_flags & SAFETY_AI_FLAG_FIRE) {
    // AI 看到火还不够，必须传感器（烟感/温感）也确认
    if (safety_sensor_rise_high(state, stm32, now_ms) ||  // 传感器上升趋势
        env_abs_high ||                                    // 环境绝对值超标
        state->fire_env_confirm_until_ms > now_ms) {       // 8秒持续确认窗口
        state->fire_locked = 1;  // 锁定火警
    }
}
```

为什么：YOLO 对火光的误检率较高（反光衣反光、焊接火花、手电筒），单靠 AI 容易误触发。加上 STM32 的烟感/温感做双因子确认，牺牲 8 秒的响应时间换零误报。

**2. 电子围栏动态识别**

`zone_detector.c` 通过颜色分割识别画面中实际画出的蓝色工作区和红色危险区边框：
```
蓝色连通域 → 工作区 (work_zone)
红色连通域 → 危险区 (danger_zone)
红色区必须在蓝色区内 → 否则不生效
识别成功后锁定 → 不随画面变化漂移
```

**3. PPE 检测有确认延迟**

```c
// main.c:1291-1296
if (now_ms - state->ppe_first_seen_ms < 5000) {
    state->suppress_fusion_send = 1;   // 5秒内不报警
}
```
避免人员临时经过摄像头触发瞬间报警。

**4. 手动复位后的火警抑制窗口**

```c
// main.c:1049
state->fire_ignore_until_ms = now_ms + 5000;  // 复位后 5 秒内忽略火警
```

---

### Q11: 共享内存的无锁读写是怎么实现的？

**答**：用 `version` 字段的 LSB 做自旋锁（SeqLock 模式）。

```c
// 写端 (RKNN 推理线程, rknn_worker.cc:120-150)
__sync_fetch_and_add(&g_shared_state->version, 1);  // version 变奇数 (写锁)
// ... 写入 boxes[0..N], box_count, timestamp ...
g_shared_state->valid = 1;
__sync_fetch_and_add(&g_shared_state->version, 1);  // version 变偶数 (释放)

// 读端 (父进程, main.c:640-664)
for (tries = 0; tries < 12; tries++) {
    before = shared->version;
    if (before & 1) continue;           // 奇数=写者正在写，跳过
    __sync_synchronize();               // 内存屏障
    memcpy(snapshot, shared, sizeof(*snapshot));  // 整块拷贝
    __sync_synchronize();
    after = shared->version;
    if (before == after && !(after & 1) && snapshot->valid)
        return 1;                       // version 前后一致 → 快照完整
}
```

**为什么不用 mutex**：推理线程持有锁时间极短（<1μs 写几个数组元素），用 mutex 的上下文切换开销（~3μs）反而更大。SeqLock 在写者极少、读者极多的场景下是最优选择——这和 Linux 内核的 `struct timekeeper` 用的一样。

---

## 五、流媒体与编码

### Q12: 编码器参数是怎么选的？

**答**：编码器用 `h264_rkmpp`（RK3588 MPP 硬件 H.264 编码器），参数选择的原则是**低延迟优先，兼顾画质**。

```c
// encoder.c:129-153
s->enc_ctx->bit_rate = 3000000;          // 3Mbps CBR
s->enc_ctx->rc_max_rate = 3000000;       // 与 bit_rate 一致 = CBR
s->enc_ctx->gop_size = fps / 2;          // 15帧 (0.5秒一个I帧)
s->enc_ctx->rc_buffer_size = 200000;     // 200KB 缓冲区
s->enc_ctx->qmin = 18;                   // 最低质量不设太高
s->enc_ctx->qmax = 38;                   // 最高质量放宽 (避免复杂画面码率爆炸)
s->enc_ctx->max_b_frames = 0;            // 不要 B 帧 (B 帧增加延迟)
```

以及 FFmpeg 级别的零延迟选项：
```c
// encoder.c:164-168
av_dict_set(&codec_opts, "tune", "zerolatency", 0);
av_dict_set(&codec_opts, "rc_mode", "CBR", 0);
av_dict_set(&codec_opts, "profile", "baseline", 0);  // baseline = 无 B 帧
av_dict_set(&codec_opts, "bf", "0", 0);
```

**比特率为什么当前配置为 3Mbps？** 1920×1080@60fps 的 NV12 裸流约为 186.6MB/s（1.49Gbps），3Mbps 是当前网络与存储预算下的 CBR 配置，不应表述为所有场景的画质底线。复杂运动、烟火或多人场景仍需用目标画面做码率 A/B，并按检测精度和块效应调整。

### Q13: RTMP 推流断了怎么处理？

**答**：三级容错机制。

```
第1级：子进程内切换 (RTMP → 本地 .ts 文件)
  encoder.c — av_write_frame 失败 → 切到本地 mpegts 文件

第2级：子进程退出 + 父进程指数退避重启
  main.c — 每 30s 轮转本地文件时顺带尝试恢复 RTMP
  退避间隔: 1s → 2s → 4s → 8s → 16s → 30s (封顶)
  一旦成功 → 重置回 1s

第3级：视频补传 (video_uploader)
  本地 .ts 分片在 SQLite 中登记索引 (video_segments 表)
  video_uploader 线程扫描 state='pending' 的片段 HTTP 补传到云端
  上传成功 → state='uploaded'
```

---

## 六、OSD 显示

### Q14: OSD 文字为什么直接写 NV12 Y 平面而不是用 RGA alpha blend？

**答**：实践踩坑的结论。

**RGA alpha blend 方案**：
- 优势：支持 RGBA 贴图，可以做半透明效果
- 问题：RGBA→NV12 的 alpha blend 在 YUV420 色度子采样下，小字（14-20px）边缘会出现**彩色花边和噪点**。原因：UV 平面是 420 子采样（每 2x2 像素共用一个 UV），小字的 RGBA 边缘像素在做 chroma 混合时把相邻像素的颜色"拉"了过来
- 尤其严重的是：经过 H.264 压缩后，这些 UV 噪点被编码器进一步劣化，RTMP/VLC 播放端看起来像彩色雪花

**直接写 Y 平面方案**（当前做法）：
```c
// encoder.c:568-597 — 遍历 GlyphStamp 的 RGBA 数据
for (row = 0; row < stamp->height; row++) {
    for (col = 0; col < stamp->width; col++) {
        if (stamp->rgba_data[src_index + 3] != 0) {              // alpha 非透明
            y_plane[dst_y * linesize + dst_x] = 235;             // 只写 Y，不碰 UV
        }
    }
}
```
- Y=235（白色），UV 不动（保持背景的 UV 值 = 128，即灰色/无色）
- 结果：白字，无色度分量，H.264 压缩后不会有彩色边缘
- 代价：不能做彩色文字，但在这个场景下不需要

**黑色底条**：
```c
nv12_fill_rect(s, bg_x, bg_y, bg_w, bg_h, 16, 128);  // Y=16 (黑), UV=128 (灰=无色)
```

**总结原则**：在 NV12 上做 OSD，标签/文字只写 Y 平面（单色亮度），不要碰 UV 平面。UV 平面保持 128（中性灰=无色），避免 YUV420 色度子采样导致的彩色噪点。

---

## 七、编译与部署

### Q15: 交叉编译环境是怎么搭的？

**答**：Makefile 中显式指定交叉编译器和所有库路径。

```makefile
# Makefile 关键配置
CROSS_COMPILE = .../aarch64-none-linux-gnu-       # ARM64 交叉编译器
CC  = $(CROSS_COMPILE)gcc
CXX = $(CROSS_COMPILE)g++

# 依赖库 (全部预编译好的 aarch64 版本)
FFmpeg  → libavformat.so.60 / libavcodec.so.60    # RTMP 推流 + H.264 编码
RKNN    → librknnrt                                 # NPU 推理运行时
RGA     → librga / IM2D                             # 2D 硬件加速
FreeType → libfreetype                              # 字体渲染 (OSD 字模)
MQTT    → libpaho-mqtt3c                            # 阿里云 IoT MQTT
Modbus  → libmodbus                                 # 传感器采集
SQLite  → libsqlite3                                # 离线缓存
OpenCV  → libopencv_imgproc / libopencv_core         # 区域识别中的图像处理
```

**为什么用共享库而不是静态链接**：RK3588 板端 Flash 空间有限，多个应用共享库节省空间。而且 FFmpeg 的 `libavcodec.so` 动态加载 `h264_rkmpp` 编码器插件，静态链接反而麻烦。

**部署**：`deploy_main_once.exp` 用 expect 脚本 scp 上传编译出的 `main` 二进制到板端 `/root/` 目录。

---

## 八、踩坑总结

### Q16: 整个开发过程中踩过哪些关键坑？

| 坑 | 现象 | 根因 | 解决方案 |
|----|------|------|----------|
| MPP 编码器崩溃 | `avcodec_free_context` 后 kernel Oops | MPP 驱动引用计数 bug | 不调用 free，让子进程退出回收 |
| RGA 画框失败 | `Error yuv not align to 2` | NV12 的 UV 交错要求 2 对齐 | 画框前坐标对齐到偶数 + 改用 NV12 直接画线 |
| 量化模型无输出 | `candidates=0 boxes=0` | INT8 scale 被 bbox 大范围拉偏 | 改用 FP16 模型 |
| OSD 小字彩边 | RTMP 播放时汉字有彩色噪点 | RGBA→NV12 alpha blend 色度子采样 | 直接写 Y 平面，UV 保持 128 |
| 时间戳错乱 | 本地录像文件命名 `1970-01-01` | CLOCK_MONOTONIC 误当成 CLOCK_REALTIME | 拆分两种时钟：退避用单调钟，文件名用实时钟 |
| 离线缓存路径混乱 | SQL 查到 0 条但日志显示已保存 | 查错了数据库文件 | 以程序启动日志中的路径为准 |
| QP 过高导致码率爆炸 | 复杂画面时推流卡顿 | qmax=45 时场景切换码率失控 | 降低 qmax 到 38 |
| sensor fps 锁定不稳定 | 曝光参数每帧被 ISP 重置 | ISP AE 在 sensor subdev 上持续覆盖 | 每 5s 重新写一次 V4L2 control（定时锁） |
