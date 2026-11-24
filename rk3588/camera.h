#ifndef CAMERA_H
#define CAMERA_H

#include <stddef.h>

#define BUF_COUNT 4

struct Buffer {
    void *start;
    size_t length;
};

typedef struct {
    int fd;
    struct Buffer buffers[BUF_COUNT];
    int buf_type; /* V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE */
    int width;         /* 驱动实际协商出的宽 */
    int height;        /* 驱动实际协商出的高 */
    size_t frame_bytes;/* 驱动实际要求的单帧字节数 */
    double fps;        /* 驱动实际帧率（0 表示驱动未报告） */
} CameraCtx;

/* desired_fps: 期望帧率；<=0 表示不关心。
 * 帧率由 sensor 模式决定，驱动不支持 S_PARM 时本函数会报告实际值，
 * 调用者必须以 ctx->fps 为准，而不是沿用自己期望的数值。 */
int camera_init(CameraCtx *ctx, const char *dev_name, int width, int height, int desired_fps);
/* 查询 sensor 实际帧率（帧率由 sensor 模式决定），供父/子进程统一取用 */
double camera_query_actual_fps(void);
int camera_start(CameraCtx *ctx);
int camera_stop(CameraCtx *ctx);
void camera_deinit(CameraCtx *ctx);

#endif
