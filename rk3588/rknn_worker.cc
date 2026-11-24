/*
 * RKNN 推理 worker（多 NPU 核心版）
 *
 * 设计依据（RK3588 / librknnrt 2.1.0 实测）：
 *   - 单上下文 rknn_set_core_mask(CORE_0_1_2)：单帧耗时几乎不变（152ms → 147ms），
 *     说明单次推理无法被 3 核分摊。
 *   - 3 个上下文各绑一核并行：吞吐 2.4× (fp16) / 2.8× (int8)，
 *     所以采用「多上下文 + 绑核 + 每上下文独立输入缓冲与工作线程」。
 *
 * 数据流：每上下文 SLOTS_PER_CTX 个 dma-buf 输入槽（rknn_create_mem），
 *        生产者 RGA 填好后 submit 到该上下文的队列，其工作线程独自推理。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <linux/types.h>
#include <errno.h>

#ifndef DMA_BUF_IOCTL_SYNC
struct dma_buf_sync {
    __u64 flags;
};
#define DMA_BUF_SYNC_READ      (1 << 0)
#define DMA_BUF_SYNC_WRITE     (2 << 0)
#define DMA_BUF_SYNC_RW        (DMA_BUF_SYNC_READ | DMA_BUF_SYNC_WRITE)
#define DMA_BUF_SYNC_START     (0 << 2)
#define DMA_BUF_SYNC_END       (1 << 2)
#define DMA_BUF_BASE           'b'
#define DMA_BUF_IOCTL_SYNC     _IOW(DMA_BUF_BASE, 0, struct dma_buf_sync)
#endif

#include "rknn_api.h"
#include "rknn_worker.h"
#include "postprocess.h"
#include "Float16.h"

#define MAX_CTX          4   /* RK3588 实际 3 核，留 1 个余量 */
#define SLOTS_PER_CTX    2   /* 每上下文输入槽：推理中 1 个 + 预填 1 个 */
#define QUEUE_DEPTH      2

typedef struct {
    int slot;              /* 上下文内的槽位 */
    int64_t frame_seq;
    int64_t frame_wall_ms;
} RknnJob;

typedef struct {
    rknn_context ctx;
    rknn_tensor_mem *mems[SLOTS_PER_CTX];
    int slot_in_use[SLOTS_PER_CTX];
    RknnJob jobs[QUEUE_DEPTH];
    int head, tail, count;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    pthread_t thread;
    int thread_started;
    rknn_core_mask mask;
    uint64_t processed;
    int64_t last_infer_us;
} RknnCtx;

static RknnCtx g_ctxs[MAX_CTX];
static int g_num_ctx = 1;
static int g_rr_ctx = 0;                       /* 轮询分配上下文用 */

static rknn_input_output_num io_num;
static rknn_tensor_attr *input_attrs = NULL;
static rknn_tensor_attr *output_attrs = NULL;
static DetectSharedState *g_shared_state = NULL;
static float g_conf_threshold = 0.5f;
static float g_nms_threshold = 0.45f;
static volatile int g_stop = 0;
static RknnWorkerStats g_stats;
static pthread_mutex_t g_stats_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static uint32_t input_rgb_u8_size(void)
{
    if (input_attrs == NULL || input_attrs[0].n_dims < 4) {
        return 0;
    }
    return (uint32_t)(input_attrs[0].dims[1] * input_attrs[0].dims[2] * input_attrs[0].dims[3]);
}

static int input_is_fp16(void)
{
    return input_attrs != NULL && input_attrs[0].type == RKNN_TENSOR_FLOAT16;
}

static void convert_rgb_u8_to_fp16_inplace(void *buffer, uint32_t rgb_size)
{
    uint8_t *rgb = (uint8_t *)buffer;
    rknpu2::float16 *fp16 = (rknpu2::float16 *)buffer;

    for (int64_t i = (int64_t)rgb_size - 1; i >= 0; --i) {
        fp16[i] = (float)rgb[i] / 255.0f;
    }
}

static void stats_add(uint64_t *field)
{
    pthread_mutex_lock(&g_stats_lock);
    (*field)++;
    pthread_mutex_unlock(&g_stats_lock);
}

static void stats_dropped(void)
{
    pthread_mutex_lock(&g_stats_lock);
    g_stats.dropped++;
    pthread_mutex_unlock(&g_stats_lock);
}

static void write_detect_results(const object_detect_result_list *od_results,
                                int64_t frame_seq,
                                int64_t frame_wall_ms)
{
    if (g_shared_state == NULL || od_results == NULL) {
        return;
    }
    /* 多上下文并行会有乱序返回：只允许更新更新的帧，避免旧结果覆盖新结果 */
    if (g_shared_state->valid && frame_seq <= g_shared_state->frame_seq) {
        return;
    }

    __sync_fetch_and_add(&g_shared_state->version, 1);
    g_shared_state->frame_seq = frame_seq;
    g_shared_state->timestamp_ms = frame_wall_ms;
    g_shared_state->box_count = (od_results->count > DETECT_MAX_BOXES) ? DETECT_MAX_BOXES : od_results->count;

    for (int i = 0; i < g_shared_state->box_count; i++) {
        g_shared_state->boxes[i].x1 = (float)od_results->results[i].box.left;
        g_shared_state->boxes[i].y1 = (float)od_results->results[i].box.top;
        g_shared_state->boxes[i].x2 = (float)od_results->results[i].box.right;
        g_shared_state->boxes[i].y2 = (float)od_results->results[i].box.bottom;
        g_shared_state->boxes[i].score = od_results->results[i].prop;
        g_shared_state->boxes[i].class_id = od_results->results[i].cls_id;
    }
    g_shared_state->valid = 1;
    __sync_fetch_and_add(&g_shared_state->version, 1);
}

static void release_slot(RknnCtx *c, int slot)
{
    if (slot < 0 || slot >= SLOTS_PER_CTX) {
        return;
    }
    pthread_mutex_lock(&c->lock);
    c->slot_in_use[slot] = 0;
    pthread_mutex_unlock(&c->lock);
}

static void process_job(RknnCtx *c, const RknnJob *job)
{
    int64_t start_us = now_us();
    rknn_input in;
    rknn_output *outputs = NULL;
    object_detect_result_list od_results;
    struct dma_buf_sync sync = {0};

    if (job->slot < 0 || job->slot >= SLOTS_PER_CTX || c->mems[job->slot] == NULL) {
        stats_add(&g_stats.failed);
        return;
    }

    sync.flags = DMA_BUF_SYNC_START | DMA_BUF_SYNC_RW;
    ioctl(c->mems[job->slot]->fd, DMA_BUF_IOCTL_SYNC, &sync);

    memset(&in, 0, sizeof(in));
    in.index = 0;
    in.fmt = RKNN_TENSOR_NHWC;
    in.buf = c->mems[job->slot]->virt_addr;
    if (input_rgb_u8_size() == 0) {
        stats_add(&g_stats.failed);
        sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
        ioctl(c->mems[job->slot]->fd, DMA_BUF_IOCTL_SYNC, &sync);
        goto out;
    }
    if (input_is_fp16()) {
        convert_rgb_u8_to_fp16_inplace(c->mems[job->slot]->virt_addr, input_rgb_u8_size());
        in.type = RKNN_TENSOR_FLOAT16;
        in.size = input_attrs[0].size;
        in.pass_through = 1;
    } else {
        in.type = RKNN_TENSOR_UINT8;
        in.size = input_rgb_u8_size();
        in.pass_through = 0;
    }

    sync.flags = DMA_BUF_SYNC_END | DMA_BUF_SYNC_RW;
    ioctl(c->mems[job->slot]->fd, DMA_BUF_IOCTL_SYNC, &sync);

    if (rknn_inputs_set(c->ctx, io_num.n_input, &in) < 0) {
        stats_add(&g_stats.failed);
        goto out;
    }
    if (rknn_run(c->ctx, NULL) < 0) {
        stats_add(&g_stats.failed);
        goto out;
    }

    outputs = (rknn_output *)calloc(io_num.n_output, sizeof(rknn_output));
    if (outputs == NULL) {
        stats_add(&g_stats.failed);
        goto out;
    }
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        outputs[i].want_float = 1;
    }

    if (rknn_outputs_get(c->ctx, io_num.n_output, outputs, NULL) < 0) {
        stats_add(&g_stats.failed);
        goto out;
    }

    post_process(outputs, &output_attrs[0], io_num.n_output,
                 g_conf_threshold, g_nms_threshold, &od_results);
    rknn_outputs_release(c->ctx, io_num.n_output, outputs);

    write_detect_results(&od_results, job->frame_seq, job->frame_wall_ms);

    pthread_mutex_lock(&g_stats_lock);
    g_stats.processed++;
    g_stats.decoded_candidates = od_results.count;
    g_stats.last_infer_us = now_us() - start_us;
    pthread_mutex_unlock(&g_stats_lock);

out:
    if (outputs != NULL) {
        free(outputs);
    }
    release_slot(c, job->slot);
}

static void *ctx_thread_main(void *arg)
{
    RknnCtx *c = (RknnCtx *)arg;

    for (;;) {
        RknnJob job;

        pthread_mutex_lock(&c->lock);
        while (!g_stop && c->count == 0) {
            pthread_cond_wait(&c->cond, &c->lock);
        }
        if (c->count == 0) {
            pthread_mutex_unlock(&c->lock);
            if (g_stop) {
                break;
            }
            continue;
        }
        job = c->jobs[c->head];
        c->head = (c->head + 1) % QUEUE_DEPTH;
        c->count--;
        pthread_mutex_unlock(&c->lock);

        process_job(c, &job);
    }
    return NULL;
}

extern "C" void rknn_worker_config_defaults(RknnWorkerConfig *config)
{
    memset(config, 0, sizeof(*config));
    config->model_path = "/root/model/best.rknn";
    config->num_ctx = 3;
    config->conf_threshold = 0.50f;
    config->nms_threshold = 0.45f;
}

extern "C" int rknn_worker_start(RknnWorker *worker, RknnWorkerConfig *config)
{
    static const rknn_core_mask masks[MAX_CTX] = {
        RKNN_NPU_CORE_0, RKNN_NPU_CORE_1, RKNN_NPU_CORE_2, RKNN_NPU_CORE_AUTO
    };
    FILE *fp;
    long size;
    unsigned char *data;
    int ret;
    int n;

    (void)worker;

    memset(&g_stats, 0, sizeof(g_stats));
    memset(g_ctxs, 0, sizeof(g_ctxs));
    g_stop = 0;
    g_rr_ctx = 0;

    g_shared_state = config->detect_state;
    g_conf_threshold = config->conf_threshold;
    g_nms_threshold = config->nms_threshold;

    n = config->num_ctx;
    if (n < 1) {
        n = 1;
    }
    if (n > MAX_CTX) {
        n = MAX_CTX;
    }
    g_num_ctx = n;

    fp = fopen(config->model_path, "rb");
    if (fp == NULL) {
        fprintf(stderr, "[AI] model load failed: %s\n", config->model_path);
        return -1;
    }
    fseek(fp, 0, SEEK_END);
    size = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    data = (unsigned char *)malloc((size_t)size);
    if (data == NULL) {
        fclose(fp);
        return -1;
    }
    if (fread(data, 1, (size_t)size, fp) != (size_t)size) {
        fclose(fp);
        free(data);
        return -1;
    }
    fclose(fp);

    ret = rknn_init(&g_ctxs[0].ctx, data, (uint32_t)size, 0, NULL);
    free(data);
    if (ret < 0) {
        fprintf(stderr, "[AI] rknn_init failed ret=%d\n", ret);
        return -1;
    }

    if (rknn_query(g_ctxs[0].ctx, RKNN_QUERY_IN_OUT_NUM, &io_num, sizeof(io_num)) != RKNN_SUCC) {
        fprintf(stderr, "[AI] query io num failed\n");
        return -1;
    }
    input_attrs = (rknn_tensor_attr *)calloc(io_num.n_input, sizeof(rknn_tensor_attr));
    output_attrs = (rknn_tensor_attr *)calloc(io_num.n_output, sizeof(rknn_tensor_attr));
    if (input_attrs == NULL || output_attrs == NULL) {
        return -1;
    }
    for (uint32_t i = 0; i < io_num.n_input; i++) {
        input_attrs[i].index = i;
        rknn_query(g_ctxs[0].ctx, RKNN_QUERY_INPUT_ATTR, &input_attrs[i], sizeof(rknn_tensor_attr));
    }
    for (uint32_t i = 0; i < io_num.n_output; i++) {
        output_attrs[i].index = i;
        rknn_query(g_ctxs[0].ctx, RKNN_QUERY_OUTPUT_ATTR, &output_attrs[i], sizeof(rknn_tensor_attr));
    }

    /* 复制上下文并绑核：dup 共享模型权重，初始化开销远小于再 init 一次 */
    for (int i = 1; i < g_num_ctx; i++) {
        if (rknn_dup_context(&g_ctxs[0].ctx, &g_ctxs[i].ctx) != RKNN_SUCC || g_ctxs[i].ctx == 0) {
            fprintf(stderr, "[AI] rknn_dup_context[%d] failed, 降级为 %d 个上下文\n", i, i);
            g_num_ctx = i;
            break;
        }
    }

    for (int i = 0; i < g_num_ctx; i++) {
        RknnCtx *c = &g_ctxs[i];

        c->mask = masks[i];
        if (rknn_set_core_mask(c->ctx, c->mask) != RKNN_SUCC) {
            fprintf(stderr, "[AI] ctx[%d] set_core_mask failed（继续，可能落在默认核）\n", i);
        }
        for (int s = 0; s < SLOTS_PER_CTX; s++) {
            c->mems[s] = rknn_create_mem(c->ctx, input_attrs[0].size);
            if (c->mems[s] == NULL) {
                fprintf(stderr, "[AI] ctx[%d] rknn_create_mem[%d] failed\n", i, s);
                return -1;
            }
        }
        pthread_mutex_init(&c->lock, NULL);
        pthread_cond_init(&c->cond, NULL);
        c->head = c->tail = c->count = 0;

        if (pthread_create(&c->thread, NULL, ctx_thread_main, c) != 0) {
            fprintf(stderr, "[AI] ctx[%d] pthread_create failed\n", i);
            return -1;
        }
        c->thread_started = 1;
    }

    printf("[AI] RKNN worker started: ctx=%d slots/ctx=%d queue=%d tensor=%u rgb_u8=%u fp16=%d conf=%.2f nms=%.2f model=%s\n",
           g_num_ctx, SLOTS_PER_CTX, QUEUE_DEPTH, input_attrs[0].size,
           input_rgb_u8_size(), input_is_fp16(), g_conf_threshold, g_nms_threshold,
           config->model_path);
    for (int i = 0; i < g_num_ctx; i++) {
        printf("[AI]   ctx[%d] core_mask=0x%x\n", i, (unsigned)g_ctxs[i].mask);
    }
    return 0;
}

extern "C" int rknn_worker_acquire_input_buffer(RknnWorker *worker, RknnWorkerInputBuffer *buffer)
{
    (void)worker;

    if (buffer == NULL || input_attrs == NULL || g_num_ctx <= 0) {
        return EINVAL;
    }

    /* 轮询各上下文，找空闲槽位；都忙则返回 EBUSY 让调用方丢帧 */
    for (int k = 0; k < g_num_ctx; k++) {
        int idx = (g_rr_ctx + k) % g_num_ctx;
        RknnCtx *c = &g_ctxs[idx];

        pthread_mutex_lock(&c->lock);
        if (!g_stop && c->count < QUEUE_DEPTH) {
            for (int s = 0; s < SLOTS_PER_CTX; s++) {
                if (!c->slot_in_use[s]) {
                    c->slot_in_use[s] = 1;
                    pthread_mutex_unlock(&c->lock);

                    buffer->slot = idx * SLOTS_PER_CTX + s;
                    buffer->fd = c->mems[s]->fd;
                    buffer->virt_addr = c->mems[s]->virt_addr;
                    buffer->width = input_attrs[0].dims[2];
                    buffer->height = input_attrs[0].dims[1];
                    buffer->channels = input_attrs[0].dims[3];
                    buffer->size = input_rgb_u8_size();
                    g_rr_ctx = (idx + 1) % g_num_ctx;
                    return 0;
                }
            }
        }
        pthread_mutex_unlock(&c->lock);
    }
    return EBUSY;
}

extern "C" int rknn_worker_submit_input_buffer(RknnWorker *worker, int slot, int64_t frame_seq, int64_t frame_wall_ms)
{
    RknnCtx *c;
    int ctx_idx;
    int local_slot;

    (void)worker;

    if (slot < 0 || slot >= g_num_ctx * SLOTS_PER_CTX) {
        return EINVAL;
    }
    ctx_idx = slot / SLOTS_PER_CTX;
    local_slot = slot % SLOTS_PER_CTX;
    c = &g_ctxs[ctx_idx];

    pthread_mutex_lock(&c->lock);
    if (g_stop) {
        c->slot_in_use[local_slot] = 0;
        pthread_mutex_unlock(&c->lock);
        return ECANCELED;
    }
    if (c->count >= QUEUE_DEPTH) {
        c->slot_in_use[local_slot] = 0;
        pthread_mutex_unlock(&c->lock);
        stats_dropped();
        return EBUSY;
    }
    c->jobs[c->tail].slot = local_slot;
    c->jobs[c->tail].frame_seq = frame_seq;
    c->jobs[c->tail].frame_wall_ms = frame_wall_ms;
    c->tail = (c->tail + 1) % QUEUE_DEPTH;
    c->count++;
    pthread_cond_signal(&c->cond);
    pthread_mutex_unlock(&c->lock);

    stats_add(&g_stats.submitted);
    return 0;
}

extern "C" void rknn_worker_release_input_buffer(RknnWorker *worker, int slot)
{
    (void)worker;
    if (slot < 0 || slot >= g_num_ctx * SLOTS_PER_CTX) {
        return;
    }
    release_slot(&g_ctxs[slot / SLOTS_PER_CTX], slot % SLOTS_PER_CTX);
}

extern "C" int rknn_worker_is_ready(RknnWorker *worker)
{
    (void)worker;
    return g_num_ctx > 0 && g_ctxs[0].thread_started;
}

extern "C" int rknn_worker_get_input_info(RknnWorker *worker, RknnWorkerInputInfo *info)
{
    (void)worker;
    if (info == NULL || input_attrs == NULL) {
        return EINVAL;
    }
    info->width = input_attrs[0].dims[2];
    info->height = input_attrs[0].dims[1];
    info->channels = input_attrs[0].dims[3];
    info->size = input_rgb_u8_size();
    return 0;
}

extern "C" void rknn_worker_get_stats(RknnWorker *worker, RknnWorkerStats *stats)
{
    (void)worker;
    if (stats == NULL) {
        return;
    }
    pthread_mutex_lock(&g_stats_lock);
    *stats = g_stats;
    pthread_mutex_unlock(&g_stats_lock);
}

extern "C" void rknn_worker_stop(RknnWorker *worker)
{
    (void)worker;

    if (g_num_ctx <= 0) {
        return;
    }
    pthread_mutex_lock(&g_stats_lock);
    g_stop = 1;
    pthread_mutex_unlock(&g_stats_lock);

    for (int i = 0; i < g_num_ctx; i++) {
        RknnCtx *c = &g_ctxs[i];
        pthread_mutex_lock(&c->lock);
        pthread_cond_signal(&c->cond);
        pthread_mutex_unlock(&c->lock);
        if (c->thread_started) {
            pthread_join(c->thread, NULL);
            c->thread_started = 0;
        }
    }
    for (int i = 0; i < g_num_ctx; i++) {
        for (int s = 0; s < SLOTS_PER_CTX; s++) {
            if (g_ctxs[i].mems[s] != NULL) {
                rknn_destroy_mem(g_ctxs[i].ctx, g_ctxs[i].mems[s]);
                g_ctxs[i].mems[s] = NULL;
            }
        }
        if (g_ctxs[i].ctx != 0) {
            rknn_destroy(g_ctxs[i].ctx);
            g_ctxs[i].ctx = 0;
        }
    }
    g_num_ctx = 0;
}
