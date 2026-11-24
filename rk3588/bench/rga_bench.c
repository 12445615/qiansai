/* RGA 预处理耗时量化：NV12 源 → RGB888 640x640（letterbox）
 * 对比两种源尺寸：2112x1568（主路）vs 640x480（selfpath）
 * 缓冲均为 dmabuf（rknn_create_mem），与 app 的 V4L2 dmabuf → NPU dmabuf 路径一致 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <sys/time.h>
#include "rknn_api.h"
#include "im2d.h"
#include "RgaUtils.h"

static int64_t now_us(void){ struct timeval tv; gettimeofday(&tv,NULL); return (int64_t)tv.tv_sec*1000000LL+tv.tv_usec; }

#define DST_W 640
#define DST_H 640

/* 与 app 中 ai_preprocess_dma_to_rgb_fd 相同的 letterbox 逻辑 */
static int do_preprocess(int src_fd, int sw, int sh, int dst_fd, int stretch)
{
    rga_buffer_t src = wrapbuffer_fd(src_fd, sw, sh, RK_FORMAT_YCbCr_420_SP);
    rga_buffer_t dst = wrapbuffer_fd(dst_fd, DST_W, DST_H, RK_FORMAT_RGB_888);
    im_rect src_rect, dst_rect;
    uint32_t rw, rh, left, top;
    int status;

    src.wstride = sw; src.hstride = sh;
    dst.wstride = DST_W; dst.hstride = DST_H;

    if (stretch) {
        rw = DST_W; rh = DST_H; left = 0; top = 0;
    } else if ((uint64_t)DST_W * sh <= (uint64_t)DST_H * sw) {
        rw = DST_W; rh = (uint32_t)(((uint64_t)sh * DST_W) / sw);
        left = 0; top = (DST_H - rh) / 2;
    } else {
        rh = DST_H; rw = (uint32_t)(((uint64_t)sw * DST_H) / sh);
        top = 0; left = (DST_W - rw) / 2;
    }

    src_rect = (im_rect){0, 0, sw, sh};
    dst_rect = (im_rect){(int)left, (int)top, (int)rw, (int)rh};
    status = improcess(src, dst, (rga_buffer_t){0}, src_rect, dst_rect, (im_rect){0}, IM_SYNC);
    return status == IM_STATUS_SUCCESS ? 0 : status;
}

static void bench(const char *name, int sw, int sh, int stretch, int iters)
{
    int sz = 0;
    FILE *f = fopen("/root/model/best.rknn", "rb");
    if (!f) { printf("no model\n"); exit(1); }
    fseek(f, 0, SEEK_END); sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *d = malloc(sz); if (fread(d,1,sz,f) != (size_t)sz) exit(1); fclose(f);
    rknn_context ctx;
    if (rknn_init(&ctx, d, sz, 0, NULL) < 0) { printf("rknn_init fail\n"); exit(1); }
    free(d);

    size_t src_size = (size_t)sw * sh * 3 / 2;
    size_t dst_size = (size_t)DST_W * DST_H * 3;
    rknn_tensor_mem *sm = rknn_create_mem(ctx, (uint32_t)src_size);
    rknn_tensor_mem *dm = rknn_create_mem(ctx, (uint32_t)dst_size);
    if (!sm || !dm) { printf("create_mem fail\n"); exit(1); }
    memset(sm->virt_addr, 0x50, src_size);
    memset(dm->virt_addr, 0x72, dst_size);

    /* warmup */
    for (int i = 0; i < 5; i++) do_preprocess(sm->fd, sw, sh, dm->fd, stretch);

    double best = 1e9, worst = 0, sum = 0;
    for (int i = 0; i < iters; i++) {
        int64_t t = now_us();
        int rc = do_preprocess(sm->fd, sw, sh, dm->fd, stretch);
        double ms = (now_us() - t) / 1000.0;
        if (rc != 0) { printf("[rga] process failed %d\n", rc); exit(1); }
        sum += ms;
        if (ms < best) best = ms;
        if (ms > worst) worst = ms;
    }
    double avg = sum / iters;
    double mb = (src_size + dst_size) / 1048576.0;
    printf("[rga] %-28s src=%dx%d  avg=%.2f ms  min=%.2f  max=%.2f  traffic=%.2f MB  -> %.0f MB/s\n",
           name, sw, sh, avg, best, worst, mb, mb / (avg / 1000.0));
    rknn_destroy_mem(ctx, sm);
    rknn_destroy_mem(ctx, dm);
    rknn_destroy(ctx);
}

int main(int argc, char **argv)
{
    int iters = argc > 1 ? atoi(argv[1]) : 100;
    bench("mainpath letterbox", 2112, 1568, 0, iters);
    bench("selfpath letterbox",  640,  480, 0, iters);
    bench("mainpath stretch",   2112, 1568, 1, iters);
    bench("selfpath stretch",    640,  480, 1, iters);
    return 0;
}
