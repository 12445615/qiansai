#include "camera.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <linux/v4l2-subdev.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#define SENSOR_SUBDEV_ENV "CAMERA_SENSOR_SUBDEV"
#define SENSOR_SUBDEV_DEFAULT "/dev/v4l-subdev2"

/* 从 sensor subdev 读真实帧间隔：帧率由 sensor 模式决定，这是唯一可靠来源 */
static double query_sensor_fps(void)
{
    const char *subdev = getenv(SENSOR_SUBDEV_ENV);
    struct v4l2_subdev_frame_interval fi;
    int fd;

    if (subdev == NULL || subdev[0] == '\0') {
        subdev = SENSOR_SUBDEV_DEFAULT;
    }
    fd = open(subdev, O_RDWR);
    if (fd < 0) {
        return 0.0;
    }
    memset(&fi, 0, sizeof(fi));
    fi.pad = 0;
    if (ioctl(fd, VIDIOC_SUBDEV_G_FRAME_INTERVAL, &fi) == 0 &&
        fi.interval.numerator > 0 && fi.interval.denominator > 0) {
        close(fd);
        return (double)fi.interval.denominator / (double)fi.interval.numerator;
    }
    close(fd);
    return 0.0;
}

/* 从 video 节点读帧间隔（部分平台支持，失败返回 0） */
static double query_video_fps(int fd, int buf_type)
{
    struct v4l2_streamparm parm;

    memset(&parm, 0, sizeof(parm));
    parm.type = buf_type;
    if (ioctl(fd, VIDIOC_G_PARM, &parm) == 0 &&
        parm.parm.capture.timeperframe.numerator > 0 &&
        parm.parm.capture.timeperframe.denominator > 0) {
        return (double)parm.parm.capture.timeperframe.denominator /
               (double)parm.parm.capture.timeperframe.numerator;
    }
    return 0.0;
}

double camera_query_actual_fps(void)
{
    return query_sensor_fps();
}

int camera_init(CameraCtx *ctx, const char *dev_name, int width, int height, int desired_fps)
{
    struct v4l2_format fmt;
    struct v4l2_format got;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;
    struct v4l2_plane planes[1];
    double fps;
    int i;

    if (ctx == NULL || dev_name == NULL || width <= 0 || height <= 0) {
        return -1;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->fd = -1;

    ctx->fd = open(dev_name, O_RDWR);
    if (ctx->fd < 0) {
        perror("camera open");
        return -1;
    }

    ctx->buf_type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;

    /* 1) 请求格式 */
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = ctx->buf_type;
    fmt.fmt.pix_mp.width = (unsigned int)width;
    fmt.fmt.pix_mp.height = (unsigned int)height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.field = V4L2_FIELD_ANY;
    fmt.fmt.pix_mp.num_planes = 1;

    if (ioctl(ctx->fd, VIDIOC_S_FMT, &fmt) < 0) {
        perror("camera SetFormat");
        goto fail;
    }

    /* 2) 回读校验：驱动可能静默改尺寸，必须核对，不一致就失败 */
    memset(&got, 0, sizeof(got));
    got.type = ctx->buf_type;
    if (ioctl(ctx->fd, VIDIOC_G_FMT, &got) < 0) {
        perror("camera GetFormat");
        goto fail;
    }
    ctx->width = (int)got.fmt.pix_mp.width;
    ctx->height = (int)got.fmt.pix_mp.height;

    if (ctx->width != width || ctx->height != height ||
        got.fmt.pix_mp.pixelformat != V4L2_PIX_FMT_NV12) {
        fprintf(stderr,
                "[Camera] FAIL: requested %dx%d NV12, driver gives %dx%d fourcc=0x%08x\n",
                width, height, ctx->width, ctx->height,
                got.fmt.pix_mp.pixelformat);
        fprintf(stderr,
                "[Camera] 上层请求与底层实际不一致，拒绝启动（请先用 cammode.sh 把 sensor/ISP 配到同一尺寸）\n");
        goto fail;
    }
    printf("[Camera] format OK: %dx%d NV12\n", ctx->width, ctx->height);

    /* 3) 帧率：以驱动实际值为准，不再盲目写 S_PARM */
    fps = query_video_fps(ctx->fd, ctx->buf_type);
    if (fps <= 0.0) {
        fps = query_sensor_fps();
    }
    ctx->fps = fps;

    if (desired_fps > 0) {
        if (fps <= 0.0) {
            printf("[Camera] desired %d fps, driver reports no frame interval; 以此为准请用 CAMERA_FLOW_STREAM_FPS\n",
                   desired_fps);
        } else if ((int)(fps + 0.5) != desired_fps) {
            printf("[Camera] desired %d fps, pipeline fixed at %.0f fps by sensor mode %dx%d; 按 %.0f fps 运行\n",
                   desired_fps, fps, ctx->width, ctx->height, fps);
        } else {
            printf("[Camera] frame rate OK: %d fps\n", desired_fps);
        }
    }

    /* 4) 申请缓冲，并核对单帧字节数（暴露 stride 对齐差异） */
    memset(&req, 0, sizeof(req));
    req.count = BUF_COUNT;
    req.type = ctx->buf_type;
    req.memory = V4L2_MEMORY_MMAP;

    if (ioctl(ctx->fd, VIDIOC_REQBUFS, &req) < 0) {
        perror("camera request buffer");
        goto fail;
    }

    for (i = 0; i < BUF_COUNT; ++i) {
        memset(&buf, 0, sizeof(buf));
        memset(planes, 0, sizeof(planes));
        buf.type = ctx->buf_type;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = (unsigned int)i;
        buf.m.planes = planes;
        buf.length = 1;

        if (ioctl(ctx->fd, VIDIOC_QUERYBUF, &buf) < 0) {
            perror("camera query buffer");
            goto fail;
        }

        ctx->buffers[i].length = buf.m.planes[0].length;
        ctx->buffers[i].start = mmap(NULL, buf.m.planes[0].length, PROT_READ | PROT_WRITE,
                                     MAP_SHARED, ctx->fd, (off_t)buf.m.planes[0].m.mem_offset);
        if (ctx->buffers[i].start == MAP_FAILED) {
            ctx->buffers[i].start = NULL;
            perror("camera mmap");
            goto fail;
        }
        if (i == 0) {
            unsigned long long expect =
                (unsigned long long)ctx->width * (unsigned long long)ctx->height * 3ULL / 2ULL;
            ctx->frame_bytes = ctx->buffers[i].length;
            if (ctx->frame_bytes < expect) {
                fprintf(stderr,
                        "[Camera] FAIL: buffer %zu B < %llu B (%dx%d NV12)\n",
                        ctx->frame_bytes, expect, ctx->width, ctx->height);
                goto fail;
            }
            if (ctx->frame_bytes != expect) {
                printf("[Camera] buffer %zu B, 逻辑帧 %llu B（多出 %zu B 为行对齐 padding）\n",
                       ctx->frame_bytes, expect, ctx->frame_bytes - (size_t)expect);
            }
        }

        if (ioctl(ctx->fd, VIDIOC_QBUF, &buf) < 0) {
            perror("camera queue buf");
            goto fail;
        }
    }
    return 0;

fail:
    camera_deinit(ctx);
    return -1;
}

int camera_start(CameraCtx *ctx)
{
    return ioctl(ctx->fd, VIDIOC_STREAMON, &ctx->buf_type);
}

int camera_stop(CameraCtx *ctx)
{
    return ioctl(ctx->fd, VIDIOC_STREAMOFF, &ctx->buf_type);
}

void camera_deinit(CameraCtx *ctx)
{
    int i;

    if (ctx == NULL) {
        return;
    }
    for (i = 0; i < BUF_COUNT; i++) {
        if (ctx->buffers[i].start && ctx->buffers[i].start != MAP_FAILED) {
            munmap(ctx->buffers[i].start, ctx->buffers[i].length);
            ctx->buffers[i].start = NULL;
        }
    }
    if (ctx->fd >= 0) {
        close(ctx->fd);
        ctx->fd = -1;
    }
}
