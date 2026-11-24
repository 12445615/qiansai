#include "encoder.h"

#include <stdio.h>

#include <stdlib.h>

#include <string.h>

#include <libavutil/hwcontext_drm.h>

#include <drm/drm_fourcc.h>

#include <libavutil/imgutils.h>

#include <libavformat/avformat.h>

#include <libavutil/dict.h>

#include <sys/mman.h>

#include <time.h>

#include <RgaApi.h>

#include <im2d.h>

#include "osd_cache.h"



#define ALIGN_TO_2(x) ((x + 1) & ~1)
static int64_t get_mono_time_ms(void) {

    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;

}



static int64_t g_stream_start_ms = 0;



void streamer_set_src_size(FFmpegStreamer *s, int src_width, int src_height) {
    if (s == NULL || src_width <= 0 || src_height <= 0) {
        return;
    }
    s->src_width = src_width;
    s->src_height = src_height;
}

int streamer_init(FFmpegStreamer *s, const char *filename, int width, int height, int fps)

{

    const AVCodec *codec;

    int ret;
    s->width = width;

    s->height = height;

    /* 默认源=输出；需要缩放时用 streamer_set_src_size() 覆盖 */
    s->src_width = width;

    s->src_height = height;

    s->frame_pts = 0;
    s->side_fmt_ctx = NULL;
    s->side_video_st = NULL;

    

    g_stream_start_ms = get_mono_time_ms();



    /*
     * Muxer 必须按目标选：RTMP 走 flv，本地分片走 mpegts。
     * 历史上这里无条件写死 "flv"，于是 video_store 里的 .ts 分片实际是 FLV 数据，
     * 播放器和上传端按 mpegts 解析全部失败。
     */
    {
        const char *out_format = NULL;
        size_t name_len = strlen(filename);

        if (strncmp(filename, "rtmp://", 7) == 0) {
            out_format = "flv";
        } else if (name_len > 3 && strcmp(filename + name_len - 3, ".ts") == 0) {
            out_format = "mpegts";
        }

        avformat_alloc_output_context2(&s->fmt_ctx, NULL, out_format, filename);
    }

    if (!s->fmt_ctx) {

        fprintf(stderr, "Could not allocate output context\n");

        return -1;

    }



    // ==== Stream tuning 1: keep FFmpeg from adding any unnecessary re-encoding ====

    s->fmt_ctx->flags |= AVFMT_FLAG_FLUSH_PACKETS; 
    s->fmt_ctx->max_delay = 0; 



    codec = avcodec_find_encoder_by_name("h264_rkmpp");

    if (!codec) {

        fprintf(stderr, "Codec not found\n");

        return -1;

    }



    s->video_st = avformat_new_stream(s->fmt_ctx, codec);

    if (!s->video_st) {

        fprintf(stderr, "Could not create stream\n");

        return -1;

    }



    s->enc_ctx = avcodec_alloc_context3(codec);

    if (!s->enc_ctx) {

        fprintf(stderr, "Could not allocate codec context\n");

        return -1;

    }


    s->enc_ctx->width = width;
    s->enc_ctx->height = height;
    s->enc_ctx->time_base = (AVRational){1, fps};
    s->enc_ctx->framerate = (AVRational){fps, 1};
    
    // ========================================================
    // Low-latency encoding tuning starts here.
    // 1. Reduce GOP size to about half the frame rate to keep a fresh I-frame cadence.
    s->enc_ctx->gop_size = fps / 2;
    if (s->enc_ctx->gop_size < 1) {
        s->enc_ctx->gop_size = 1;
    }
    
    s->enc_ctx->max_b_frames = 0;
    s->enc_ctx->pix_fmt = AV_PIX_FMT_NV12;
    
    // 1. Target bitrate is 3 Mbps to keep stable image quality under normal conditions.
    s->enc_ctx->bit_rate = 3000000; 
    
    // 2. Keep the max rate at 3 Mbps to avoid sudden bursts during motion or scene changes.
    s->enc_ctx->rc_max_rate = 3000000; 
    s->enc_ctx->rc_buffer_size = 200000;

    // 3. Tune quantization bounds: qmin/qmax keep the stream smooth while avoiding excessive quality loss.
    // qmin is 18 to avoid over-compression artifacts.
    // qmax is kept at 38; a lower value would raise bit rate too much, while a higher value may degrade motion quality.
    s->enc_ctx->qmin = 18;
    s->enc_ctx->qmax = 38;
    s->video_st->time_base = s->enc_ctx->time_base;
    // ========================================================

    if (s->fmt_ctx->oformat->flags & AVFMT_GLOBALHEADER) {
        s->enc_ctx->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }



    AVDictionary *codec_opts = NULL;
    av_dict_set(&codec_opts, "tune", "zerolatency", 0);
    av_dict_set(&codec_opts, "rc_mode", "CBR", 0);
    av_dict_set(&codec_opts, "profile", "baseline", 0);
    av_dict_set(&codec_opts, "bf", "0", 0);
    av_dict_set(&codec_opts, "delay", "0", 0);

    if (avcodec_open2(s->enc_ctx, codec, &codec_opts) < 0) {

        fprintf(stderr, "Could not open codec\n");
        av_dict_free(&codec_opts);

        return -1;

    }
    av_dict_free(&codec_opts);

    avcodec_parameters_from_context(s->video_st->codecpar, s->enc_ctx);



    s->yuv_frame = av_frame_alloc();

    if (!s->yuv_frame) {

        fprintf(stderr, "Could not allocate frame\n");

        return -1;

    }



    s->yuv_frame->format = AV_PIX_FMT_NV12;

    s->yuv_frame->width = width;

    /* 帧缓冲必须按真实高度申请：后面的 hstride / UV 偏移全部由 height 推导，
     * 历史上这里写死过 1568 和 768（720p 时代的对齐高度），一旦分辨率变了
     * 就会把 UV 平面写进 Y 平面里，RGA 也跟着用错 hstride。 */
    s->yuv_frame->height = height;



    if (av_frame_get_buffer(s->yuv_frame, 64) < 0) {

        fprintf(stderr, "Could not allocate frame data.\n");

        return -1;

    }



    s->yuv_frame->height = height;



    if (!(s->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {

        AVDictionary *opts = NULL;

        av_dict_set(&opts, "rw_timeout", "2000000", 0); 

        // ==== Stream open tuning 2: avoid buffering and keep writes near real time ====

        av_dict_set(&opts, "fflags", "nobuffer", 0); 
        av_dict_set(&opts, "flush_packets", "1", 0);
        av_dict_set(&opts, "tcp_nodelay", "1", 0); 

        

        if (avio_open2(&s->fmt_ctx->pb, filename, AVIO_FLAG_WRITE, NULL, &opts) < 0) {

            fprintf(stderr, "Could not open output file\n");

            av_dict_free(&opts);

            return -1;

        }

        av_dict_free(&opts);

    }



    ret = avformat_write_header(s->fmt_ctx, NULL);

    if (ret < 0) {

        fprintf(stderr, "Could not write stream header: %d\n", ret);

        return -1;

    }



    return 0;

}




static int streamer_write_side_packet(FFmpegStreamer *s, const AVPacket *pkt)
{
    AVPacket side_pkt;
    int ret;

    if (s == NULL || s->side_fmt_ctx == NULL || s->side_video_st == NULL || pkt == NULL) {
        return 0;
    }

    av_init_packet(&side_pkt);
    ret = av_packet_ref(&side_pkt, pkt);
    if (ret < 0) {
        return ret;
    }

    av_packet_rescale_ts(&side_pkt, s->video_st->time_base, s->side_video_st->time_base);
    side_pkt.stream_index = s->side_video_st->index;
    ret = av_write_frame(s->side_fmt_ctx, &side_pkt);
    av_packet_unref(&side_pkt);
    return ret;
}

int streamer_start_side_record(FFmpegStreamer *s, const char *filename)
{
    AVFormatContext *fmt = NULL;
    AVStream *st = NULL;
    int ret;

    if (s == NULL || filename == NULL || filename[0] == '\0') {
        return -1;
    }
    if (s->side_fmt_ctx != NULL) {
        return 0;
    }
    if (s->fmt_ctx == NULL || s->video_st == NULL || s->enc_ctx == NULL) {
        return -1;
    }

    ret = avformat_alloc_output_context2(&fmt, NULL, "mpegts", filename);
    if (ret < 0 || fmt == NULL) {
        fprintf(stderr, "[Encoder] side record alloc output failed: %s\n", filename);
        return -1;
    }

    fmt->flags |= AVFMT_FLAG_FLUSH_PACKETS;
    st = avformat_new_stream(fmt, NULL);
    if (st == NULL) {
        avformat_free_context(fmt);
        return -1;
    }
    ret = avcodec_parameters_copy(st->codecpar, s->video_st->codecpar);
    if (ret < 0) {
        avformat_free_context(fmt);
        return -1;
    }
    st->codecpar->codec_tag = 0;
    st->time_base = s->video_st->time_base;

    if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
        ret = avio_open2(&fmt->pb, filename, AVIO_FLAG_WRITE, NULL, NULL);
        if (ret < 0) {
            fprintf(stderr, "[Encoder] side record open failed: %s ret=%d\n", filename, ret);
            avformat_free_context(fmt);
            return -1;
        }
    }

    ret = avformat_write_header(fmt, NULL);
    if (ret < 0) {
        fprintf(stderr, "[Encoder] side record header failed: %s ret=%d\n", filename, ret);
        if (!(fmt->oformat->flags & AVFMT_NOFILE)) {
            avio_closep(&fmt->pb);
        }
        avformat_free_context(fmt);
        return -1;
    }

    s->side_fmt_ctx = fmt;
    s->side_video_st = st;
    printf("[Encoder] side record started: %s\n", filename);
    return 0;
}

int streamer_stop_side_record(FFmpegStreamer *s)
{
    if (s == NULL || s->side_fmt_ctx == NULL) {
        return 0;
    }

    av_write_trailer(s->side_fmt_ctx);
    if (!(s->side_fmt_ctx->oformat->flags & AVFMT_NOFILE)) {
        avio_closep(&s->side_fmt_ctx->pb);
    }
    avformat_free_context(s->side_fmt_ctx);
    s->side_fmt_ctx = NULL;
    s->side_video_st = NULL;
    printf("[Encoder] side record stopped\n");
    return 0;
}

int streamer_push(FFmpegStreamer *s, uint8_t *nv12_data)

{

    int ret;

    AVPacket *pkt = av_packet_alloc();

    if (!pkt) return -1;



    s->yuv_frame->data[0] = nv12_data;

    s->yuv_frame->linesize[0] = s->width;

    s->yuv_frame->data[1] = nv12_data + s->width * s->height;

    s->yuv_frame->linesize[1] = s->width;

    

    int fps = s->enc_ctx->framerate.num > 0 ? s->enc_ctx->framerate.num : 30;

    int64_t now_ms = get_mono_time_ms();

    int64_t target_pts = (now_ms - g_stream_start_ms) * fps / 1000;

    if (target_pts <= s->frame_pts) {

        target_pts = s->frame_pts + 1;

    }

    s->frame_pts = target_pts;

    s->yuv_frame->pts = s->frame_pts;



    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);

    if (ret < 0) {

        av_packet_free(&pkt);

        return -1;

    }



    while (avcodec_receive_packet(s->enc_ctx, pkt) == 0) {

        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);

        pkt->stream_index = s->video_st->index;

        

        // ==== Stream open tuning 3: reduce packet delays so the video is sent immediately ====

        av_write_frame(s->fmt_ctx, pkt);
        streamer_write_side_packet(s, pkt);

        

        av_packet_unref(pkt);

    }



    av_packet_free(&pkt);

    return 0;

}



static int clamp_int(int value, int min_value, int max_value) {

    if (value < min_value) return min_value;

    if (value > max_value) return max_value;

    return value;

}



static int align_even_down(int value) { return value & ~1; }

static int align_even_up(int value) { return (value + 1) & ~1; }

static void nv12_fill_rect(FFmpegStreamer *s,
                           int x,
                           int y,
                           int width,
                           int height,
                           unsigned char y_value,
                           unsigned char uv_value) {
    int row;
    int uv_row;
    int uv_x;
    int uv_y;
    int uv_w;
    int uv_h;
    unsigned char *y_plane;
    unsigned char *uv_plane;

    if (s == NULL || s->yuv_frame == NULL || s->yuv_frame->data[0] == NULL ||
        width <= 0 || height <= 0) {
        return;
    }

    x = clamp_int(x, 0, s->width);
    y = clamp_int(y, 0, s->height);
    width = clamp_int(width, 0, s->width - x);
    height = clamp_int(height, 0, s->height - y);
    if (width <= 0 || height <= 0) return;

    y_plane = s->yuv_frame->data[0];
    for (row = 0; row < height; row++) {
        memset(y_plane + (y + row) * s->yuv_frame->linesize[0] + x, y_value, width);
    }

    uv_plane = s->yuv_frame->data[1];
    uv_x = x & ~1;
    uv_y = y & ~1;
    uv_w = align_even_up(width + (x - uv_x));
    uv_h = align_even_up(height + (y - uv_y)) / 2;
    uv_w = clamp_int(uv_w, 0, s->width - uv_x);
    if (uv_w <= 0 || uv_h <= 0) return;

    for (uv_row = 0; uv_row < uv_h; uv_row++) {
        memset(uv_plane + (uv_y / 2 + uv_row) * s->yuv_frame->linesize[1] + uv_x,
               uv_value,
               uv_w);
    }
}

static void nv12_fill_rect_yuv(FFmpegStreamer *s,
                               int x,
                               int y,
                               int width,
                               int height,
                               unsigned char y_value,
                               unsigned char u_value,
                               unsigned char v_value) {
    int row;
    int uv_row;
    int uv_col;
    int uv_x;
    int uv_y;
    int uv_w;
    int uv_h;
    unsigned char *y_plane;
    unsigned char *uv_plane;

    if (s == NULL || s->yuv_frame == NULL || s->yuv_frame->data[0] == NULL ||
        width <= 0 || height <= 0) {
        return;
    }

    x = clamp_int(x, 0, s->width);
    y = clamp_int(y, 0, s->height);
    width = clamp_int(width, 0, s->width - x);
    height = clamp_int(height, 0, s->height - y);
    if (width <= 0 || height <= 0) return;

    y_plane = s->yuv_frame->data[0];
    for (row = 0; row < height; row++) {
        memset(y_plane + (y + row) * s->yuv_frame->linesize[0] + x, y_value, width);
    }

    uv_plane = s->yuv_frame->data[1];
    uv_x = x & ~1;
    uv_y = y & ~1;
    uv_w = align_even_up(width + (x - uv_x));
    uv_h = align_even_up(height + (y - uv_y)) / 2;
    uv_w = clamp_int(uv_w, 0, s->width - uv_x);
    if (uv_w <= 0 || uv_h <= 0) return;

    for (uv_row = 0; uv_row < uv_h; uv_row++) {
        unsigned char *row_ptr = uv_plane + (uv_y / 2 + uv_row) * s->yuv_frame->linesize[1] + uv_x;
        for (uv_col = 0; uv_col < uv_w; uv_col += 2) {
            row_ptr[uv_col] = u_value;
            if (uv_col + 1 < uv_w) {
                row_ptr[uv_col + 1] = v_value;
            }
        }
    }
}

static void nv12_draw_stamp_luma(FFmpegStreamer *s,
                                 const GlyphStamp *stamp,
                                 int x,
                                 int y,
                                 unsigned char y_value) {
    int stride;
    int row;
    int col;
    unsigned char *y_plane;

    if (s == NULL || stamp == NULL || stamp->rgba_data == NULL ||
        s->yuv_frame == NULL || s->yuv_frame->data[0] == NULL) {
        return;
    }

    stride = (stamp->width + 3) & ~3;
    y_plane = s->yuv_frame->data[0];
    for (row = 0; row < stamp->height; row++) {
        int dst_y = y + row;
        if (dst_y < 0 || dst_y >= s->height) continue;
        for (col = 0; col < stamp->width; col++) {
            int dst_x = x + col;
            int src_index;
            if (dst_x < 0 || dst_x >= s->width) continue;
            src_index = (row * stride + col) * 4;
            if (stamp->rgba_data[src_index + 3] != 0) {
                y_plane[dst_y * s->yuv_frame->linesize[0] + dst_x] = y_value;
            }
        }
    }
}

static void draw_overlay_zone_rect(FFmpegStreamer *s,
                                   rga_buffer_t dst,
                                   const DetectZoneRect *zone,
                                   int border,
                                   unsigned int color) {
    int x1;
    int y1;
    int x2;
    int y2;
    int width;
    int height;
    im_rect rects[4];
    int edge;

    if (s == NULL || zone == NULL || !zone->valid || border <= 0) {
        return;
    }

    x1 = clamp_int((int)(zone->x1 + 0.5f), 0, s->width - 2);
    y1 = clamp_int((int)(zone->y1 + 0.5f), 0, s->height - 2);
    x2 = clamp_int((int)(zone->x2 + 0.5f), x1 + border, s->width);
    y2 = clamp_int((int)(zone->y2 + 0.5f), y1 + border, s->height);

    x1 = align_even_down(x1);
    y1 = align_even_down(y1);
    x2 = align_even_up(x2);
    y2 = align_even_up(y2);
    x2 = clamp_int(x2, x1 + border, s->width);
    y2 = clamp_int(y2, y1 + border, s->height);

    width = x2 - x1;
    height = y2 - y1;
    if (width < border || height < border) {
        return;
    }

    rects[0] = (im_rect){x1, y1, width, border};
    rects[1] = (im_rect){x1, y2 - border, width, border};
    rects[2] = (im_rect){x1, y1, border, height};
    rects[3] = (im_rect){x2 - border, y1, border, height};

    for (edge = 0; edge < 4; edge++) {
        if (imfill_t(dst, rects[edge], color, IM_SYNC) != IM_STATUS_SUCCESS) {
            return;
        }
    }
}

static void draw_nv12_line(FFmpegStreamer *s,
                           int x0,
                           int y0,
                           int x1,
                           int y1,
                           int thickness,
                           uint8_t y_value,
                           uint8_t u_value,
                           uint8_t v_value) {
    int dx;
    int dy;
    int sx;
    int sy;
    int err;

    if (s == NULL || thickness <= 0) {
        return;
    }

    x0 = clamp_int(x0, 0, s->width - 1);
    y0 = clamp_int(y0, 0, s->height - 1);
    x1 = clamp_int(x1, 0, s->width - 1);
    y1 = clamp_int(y1, 0, s->height - 1);

    dx = abs(x1 - x0);
    dy = -abs(y1 - y0);
    sx = x0 < x1 ? 1 : -1;
    sy = y0 < y1 ? 1 : -1;
    err = dx + dy;

    while (1) {
        nv12_fill_rect_yuv(s,
                           x0 - thickness / 2,
                           y0 - thickness / 2,
                           thickness,
                           thickness,
                           y_value,
                           u_value,
                           v_value);
        if (x0 == x1 && y0 == y1) {
            break;
        }
        {
            int e2 = 2 * err;
            if (e2 >= dy) {
                err += dy;
                x0 += sx;
            }
            if (e2 <= dx) {
                err += dx;
                y0 += sy;
            }
        }
    }
}

static void draw_overlay_zone_quad_red(FFmpegStreamer *s,
                                       const DetectZoneRect *zone,
                                       int thickness) {
    if (s == NULL || zone == NULL || !zone->valid) {
        return;
    }

    draw_nv12_line(s, (int)(zone->p0x + 0.5f), (int)(zone->p0y + 0.5f),
                      (int)(zone->p1x + 0.5f), (int)(zone->p1y + 0.5f),
                      thickness, 76, 84, 255);
    draw_nv12_line(s, (int)(zone->p1x + 0.5f), (int)(zone->p1y + 0.5f),
                      (int)(zone->p2x + 0.5f), (int)(zone->p2y + 0.5f),
                      thickness, 76, 84, 255);
    draw_nv12_line(s, (int)(zone->p2x + 0.5f), (int)(zone->p2y + 0.5f),
                      (int)(zone->p3x + 0.5f), (int)(zone->p3y + 0.5f),
                      thickness, 76, 84, 255);
    draw_nv12_line(s, (int)(zone->p3x + 0.5f), (int)(zone->p3y + 0.5f),
                      (int)(zone->p0x + 0.5f), (int)(zone->p0y + 0.5f),
                      thickness, 76, 84, 255);
}

static void draw_overlay_zone_quad_black(FFmpegStreamer *s,
                                         const DetectZoneRect *zone,
                                         int thickness) {
    if (s == NULL || zone == NULL || !zone->valid) {
        return;
    }

    draw_nv12_line(s, (int)(zone->p0x + 0.5f), (int)(zone->p0y + 0.5f),
                      (int)(zone->p1x + 0.5f), (int)(zone->p1y + 0.5f),
                      thickness, 16, 128, 128);
    draw_nv12_line(s, (int)(zone->p1x + 0.5f), (int)(zone->p1y + 0.5f),
                      (int)(zone->p2x + 0.5f), (int)(zone->p2y + 0.5f),
                      thickness, 16, 128, 128);
    draw_nv12_line(s, (int)(zone->p2x + 0.5f), (int)(zone->p2y + 0.5f),
                      (int)(zone->p3x + 0.5f), (int)(zone->p3y + 0.5f),
                      thickness, 16, 128, 128);
    draw_nv12_line(s, (int)(zone->p3x + 0.5f), (int)(zone->p3y + 0.5f),
                      (int)(zone->p0x + 0.5f), (int)(zone->p0y + 0.5f),
                      thickness, 16, 128, 128);
}

static void draw_detect_zones(FFmpegStreamer *s,
                              rga_buffer_t dst,
                              const ZoneOverlayState *zone_state) {
    if (zone_state == NULL || !zone_state->zone_valid) {
        return;
    }

    (void)dst;
    draw_overlay_zone_quad_red(s, &zone_state->work_zone, 4);
    draw_overlay_zone_quad_red(s, &zone_state->danger_zone, 4);
}



/* 采集坐标 → 输出坐标（例如 2112x1568 → 1056x784，比例 0.5） */
static void scale_zone_rect(DetectZoneRect *r, float sx, float sy) {
    if (r == NULL) return;
    r->x1 *= sx; r->x2 *= sx;
    r->y1 *= sy; r->y2 *= sy;
    r->p0x *= sx; r->p0y *= sy;
    r->p1x *= sx; r->p1y *= sy;
    r->p2x *= sx; r->p2y *= sy;
    r->p3x *= sx; r->p3y *= sy;
}

static void scale_detect_state(DetectSharedState *out, const DetectSharedState *in, float sx, float sy) {
    int i;
    int count;

    *out = *in;
    count = out->box_count;
    if (count > DETECT_MAX_BOXES) count = DETECT_MAX_BOXES;
    for (i = 0; i < count; i++) {
        out->boxes[i].x1 *= sx; out->boxes[i].x2 *= sx;
        out->boxes[i].y1 *= sy; out->boxes[i].y2 *= sy;
    }
    scale_zone_rect(&out->work_zone, sx, sy);
    scale_zone_rect(&out->danger_zone, sx, sy);
}

static void scale_zone_state(ZoneOverlayState *out, const ZoneOverlayState *in, float sx, float sy) {
    *out = *in;
    scale_zone_rect(&out->work_zone, sx, sy);
    scale_zone_rect(&out->danger_zone, sx, sy);
}

static void draw_detect_boxes(FFmpegStreamer *s, const DetectSharedState *detect_state, const ZoneOverlayState *zone_state) {
    DetectSharedState scaled_detect;
    ZoneOverlayState scaled_zone;

    /* 采集尺寸 != 输出尺寸时，先把叠加坐标缩放到输出坐标系 */
    if (s != NULL && s->src_width > 0 && s->src_height > 0 &&
        (s->src_width != s->width || s->src_height != s->height)) {
        float scale_x = (float)s->width / (float)s->src_width;
        float scale_y = (float)s->height / (float)s->src_height;

        if (detect_state != NULL) {
            scale_detect_state(&scaled_detect, detect_state, scale_x, scale_y);
            detect_state = &scaled_detect;
        }
        if (zone_state != NULL) {
            scale_zone_state(&scaled_zone, zone_state, scale_x, scale_y);
            zone_state = &scaled_zone;
        }
    }

    rga_buffer_t dst;

    int count;

    int i;



    if (s == NULL) return;



    dst = wrapbuffer_virtualaddr(s->yuv_frame->data[0], s->width, s->height, RK_FORMAT_YCbCr_420_SP);

    dst.wstride = s->yuv_frame->linesize[0];

    dst.hstride = (int)s->height;

    draw_detect_zones(s, dst, zone_state);

    if (detect_state == NULL || !detect_state->valid || detect_state->box_count <= 0) return;

    {
        static int64_t last_box_log_ms = 0;
        int64_t now_log_ms = get_mono_time_ms();
        if (now_log_ms - last_box_log_ms >= 1000) {
            int log_count = detect_state->box_count;
            if (log_count > 3) log_count = 3;
            printf("[Child][AIOverlay] boxes=%d frame=%lld ts=%lld\n",
                   detect_state->box_count,
                   (long long)detect_state->frame_seq,
                   (long long)detect_state->timestamp_ms);
            for (int bi = 0; bi < log_count; bi++) {
                printf("[Child][AIOverlay] #%d cls=%d score=%.3f xy=(%.1f,%.1f)-(%.1f,%.1f)\n",
                       bi,
                       detect_state->boxes[bi].class_id,
                       detect_state->boxes[bi].score,
                       detect_state->boxes[bi].x1,
                       detect_state->boxes[bi].y1,
                       detect_state->boxes[bi].x2,
                       detect_state->boxes[bi].y2);
            }
            last_box_log_ms = now_log_ms;
        }
    }

    count = detect_state->box_count;

    if (count > DETECT_MAX_BOXES) count = DETECT_MAX_BOXES;



    for (i = 0; i < count; i++) {

        int x1 = clamp_int((int)(detect_state->boxes[i].x1 + 0.5f), 0, s->width - 2);

        int y1 = clamp_int((int)(detect_state->boxes[i].y1 + 0.5f), 0, s->height - 2);

        int x2 = clamp_int((int)(detect_state->boxes[i].x2 + 0.5f), 0, s->width);

        int y2 = clamp_int((int)(detect_state->boxes[i].y2 + 0.5f), 0, s->height);

        int width = x2 - x1;

        int height = y2 - y1;

        int border = 4;
        int class_id = detect_state->boxes[i].class_id;
        im_rect rects[4];

        IM_STATUS status;

        int edge;



        if (width < border || height < border) continue;



        x1 = align_even_down(x1);

        y1 = align_even_down(y1);

        x2 = align_even_up(x2);

        y2 = align_even_up(y2);

        x2 = clamp_int(x2, x1 + border, s->width);

        y2 = clamp_int(y2, y1 + border, s->height);

        width = x2 - x1;

        height = y2 - y1;

        if (width < border || height < border) continue;



        rects[0] = (im_rect){x1, y1, width, border};

        rects[1] = (im_rect){x1, y2 - border, width, border};

        rects[2] = (im_rect){x1, y1, border, height};

        rects[3] = (im_rect){x2 - border, y1, border, height};



        (void)rects;
        (void)status;
        (void)edge;
        /* Draw AI boxes directly into NV12 so zone/RGA overlay cannot hide them. */
        draw_nv12_line(s, x1, y1, x2, y1, border, 144, 54, 34);
        draw_nv12_line(s, x2, y1, x2, y2, border, 144, 54, 34);
        draw_nv12_line(s, x2, y2, x1, y2, border, 144, 54, 34);
        draw_nv12_line(s, x1, y2, x1, y1, border, 144, 54, 34);

    }

}







int streamer_push_zerocopy_overlay(FFmpegStreamer *s, int dma_fd, const DetectSharedState *detect_state, const ZoneOverlayState *zone_state) {

    int ret;

    AVPacket *pkt;

    rga_buffer_t src, dst;

    int64_t push_start_ms;
    int64_t push_end_ms;
    static int64_t stat_start_ms = 0;
    static uint64_t stat_frames = 0;
    static int64_t stat_total_ms = 0;
    static int64_t stat_max_ms = 0;

    // Smooth detection scores over time to reduce frame-to-frame jitter.

    static float smoothed_scores[6] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};



    if (dma_fd < 0 || !s->yuv_frame->data[0]) {
        fprintf(stderr, "[Encoder] invalid frame: dma_fd=%d data0=%p\n",
                dma_fd, (void *)(s->yuv_frame ? s->yuv_frame->data[0] : NULL));
        return -1;
    }

    push_start_ms = get_mono_time_ms();
    if (stat_start_ms == 0) stat_start_ms = push_start_ms;

    // 1. Align the frame height to the expected output stride.

    ret = av_frame_make_writable(s->yuv_frame);

    if (ret < 0) {
        fprintf(stderr, "[Encoder] frame not writable: %d\n", ret);
        return -1;
    }



    /* 源按采集尺寸 wrap，目标按输出尺寸：improcess 的 drect 小于 srect 时 RGA 会缩放，
     * 2:1 缩放等价于两行平均，用来合并 sensor 60fps 隔行模式的两场（消条纹）。 */
    src = wrapbuffer_fd(dma_fd, s->src_width, s->src_height, RK_FORMAT_YCbCr_420_SP);

    dst = wrapbuffer_virtualaddr(s->yuv_frame->data[0], s->width, s->height, RK_FORMAT_YCbCr_420_SP);

    dst.wstride = s->yuv_frame->linesize[0];

    dst.hstride = (int)s->height;



    {
        IM_STATUS rga_status;

        if (s->src_width != s->width || s->src_height != s->height) {
            /* 缩小走 imresize + AVERAGE 插值：2:1 时等价于"两行平均"的 box 滤波，
             * 正好把 sensor 60fps 隔行模式的长/短曝光两场合并掉（消横向条纹）。
             * 直接 improcess 的默认插值不够平均，实测残留条纹明显。 */
            rga_status = imresize_t(src, dst, 0, 0, IM_INTERP_AVERAGE, IM_SYNC);
        } else {
            rga_status = improcess(src, dst, (rga_buffer_t){0},
                                   (im_rect){0, 0, s->width, s->height},
                                   (im_rect){0, 0, s->width, s->height},
                                   (im_rect){0}, IM_SYNC);
        }

        if (rga_status != IM_STATUS_SUCCESS) {
            fprintf(stderr,
                    "[Encoder] RGA copy/scale failed: %s (%d), src=%dx%d -> dst=%dx%d\n",
                    imStrError_t(rga_status), (int)rga_status,
                    s->src_width, s->src_height, s->width, s->height);
            return -1;
        }
    }

    

    draw_detect_boxes(s, detect_state, zone_state);



    if (detect_state != NULL && detect_state->valid && detect_state->box_count > 0) {

        DetectSharedState local = *detect_state; 

        

        for (int i = 0; i < local.box_count; i++) {

            int id = local.boxes[i].class_id;

            float raw_score = local.boxes[i].score;

            if (id < 0 || id >= 6) continue;



            // --- Apply exponential smoothing so the first frame is used directly as the baseline. ---

            if (smoothed_scores[id] < 0.0f) smoothed_scores[id] = raw_score;

            else smoothed_scores[id] = (smoothed_scores[id] * 0.8f) + (raw_score * 0.2f);

            

            float display_score = smoothed_scores[id];



            int start_x = (int)local.boxes[i].x1 & ~1;

            int start_y = ((int)local.boxes[i].y1 - 40) & ~1;

            if (start_y < 20) start_y = ((int)local.boxes[i].y2 + 5) & ~1;

            int current_x = start_x + 8;
            int score_int = (int)(display_score * 100.0f);
            if (score_int > 99) score_int = 99;
            if (score_int < 0) score_int = 0;
            GlyphStamp *label = &g_stamp_labels[id];
            GlyphStamp *score = &g_stamp_score_text[id][score_int];
            int label_w = label->rgba_data ? ALIGN_TO_2(label->width) : 0;
            int label_h = label->rgba_data ? ALIGN_TO_2(label->height) : 0;
            int score_w = score->rgba_data ? ALIGN_TO_2(score->width) : 0;
            int score_h = score->rgba_data ? ALIGN_TO_2(score->height) : 0;
            int text_h = label_h;
            if (score_h > text_h) text_h = score_h;
            int text_w = label_w + (label_w > 0 && score_w > 0 ? 5 : 0) + score_w;
            int bg_x = clamp_int(start_x - 3, 0, s->width - 2);
            int bg_y = clamp_int(start_y - 3, 0, s->height - 2);
            int bg_w = clamp_int(text_w + 8, 2, s->width - bg_x);
            int bg_h = clamp_int(text_h + 6, 2, s->height - bg_y);

            bg_x = align_even_down(bg_x);
            bg_y = align_even_down(bg_y);
            bg_w = align_even_up(bg_w);
            bg_h = align_even_up(bg_h);
            if (bg_x + bg_w > s->width) bg_w = align_even_down(s->width - bg_x);
            if (bg_y + bg_h > s->height) bg_h = align_even_down(s->height - bg_y);
            if (bg_w > 0 && bg_h > 0) {
                nv12_fill_rect(s, bg_x, bg_y, bg_w, bg_h, 16, 128);
                if (bg_w >= 8) {
                    imfill_t(dst, (im_rect){bg_x, bg_y, 6, bg_h}, osd_class_color_rgb(id), IM_SYNC);
                }
            }



            // A. Draw the class label when available.

            if (label->rgba_data) {

                nv12_draw_stamp_luma(s, label, current_x, start_y, 235);

                current_x += (label_w + 4);

            }



            // B. Draw the smoothed confidence value next to the label.

            if (score->rgba_data) {

                nv12_draw_stamp_luma(s, score, current_x, start_y, 235);
            }

        }

    }


    /* UV 紧跟在 Y 平面之后：偏移 = linesize[0] * height（按真实高度，不再用历史常量） */
    s->yuv_frame->data[1] =
        s->yuv_frame->data[0] + (int64_t)s->yuv_frame->linesize[0] * s->height;
    s->yuv_frame->linesize[1] = s->width;

    int fps = s->enc_ctx->framerate.num > 0 ? s->enc_ctx->framerate.num : 30;
    int64_t now_ms = get_mono_time_ms();
    int64_t target_pts = (now_ms - g_stream_start_ms) * fps / 1000;
    if (target_pts <= s->frame_pts) {
        target_pts = s->frame_pts + 1;
    }
    s->frame_pts = target_pts;
    s->yuv_frame->pts = s->frame_pts;
    s->yuv_frame->pkt_duration = 1;

    ret = avcodec_send_frame(s->enc_ctx, s->yuv_frame);

    if (ret < 0) {
        fprintf(stderr, "[Encoder] send_frame failed: %d\n", ret);
        return ret;
    }

    pkt = av_packet_alloc();

    if (!pkt) return -1;

    while (avcodec_receive_packet(s->enc_ctx, pkt) == 0) {

        av_packet_rescale_ts(pkt, s->enc_ctx->time_base, s->video_st->time_base);

        pkt->stream_index = s->video_st->index;

        ret = av_write_frame(s->fmt_ctx, pkt);
        if (ret < 0) {
            fprintf(stderr, "[Encoder] write_frame failed: %d (size=%d pts=%lld)\n",
                    ret, pkt->size, (long long)pkt->pts);
            av_packet_unref(pkt);
            av_packet_free(&pkt);
            return ret;
        }
        ret = streamer_write_side_packet(s, pkt);
        if (ret < 0) {
            fprintf(stderr, "[Encoder] side record packet write failed: %d\n", ret);
        }

        av_packet_unref(pkt);

    }

    av_packet_free(&pkt);

    push_end_ms = get_mono_time_ms();
    stat_frames++;
    stat_total_ms += push_end_ms - push_start_ms;
    if (push_end_ms - push_start_ms > stat_max_ms) {
        stat_max_ms = push_end_ms - push_start_ms;
    }
    if (push_end_ms - stat_start_ms >= 5000) {
        double sec = (double)(push_end_ms - stat_start_ms) / 1000.0;
        double avg_ms = stat_frames > 0 ? (double)stat_total_ms / (double)stat_frames : 0.0;
        printf("[Child][Encode] fps=%.1f avg_ms=%.2f max_ms=%lld pts=%lld\n",
               stat_frames / sec, avg_ms, (long long)stat_max_ms, (long long)s->frame_pts);
        stat_start_ms = push_end_ms;
        stat_frames = 0;
        stat_total_ms = 0;
        stat_max_ms = 0;
    }

    return 0;

}

int streamer_push_zerocopy(FFmpegStreamer *s, int dma_fd) {

    return streamer_push_zerocopy_overlay(s, dma_fd, NULL, NULL);

}



int streamer_clean(FFmpegStreamer *s)

{

    if (!s) return -1;

    streamer_stop_side_record(s);

    if (s->fmt_ctx) av_write_trailer(s->fmt_ctx);

    if (s->yuv_frame) av_frame_free(&s->yuv_frame);

    if (s->enc_ctx) avcodec_free_context(&s->enc_ctx);

    if (s->fmt_ctx && !(s->fmt_ctx->oformat->flags & AVFMT_NOFILE)) {

        avio_closep(&s->fmt_ctx->pb);

    }

    if (s->fmt_ctx) {

        avformat_free_context(s->fmt_ctx);

        s->fmt_ctx = NULL;

    }

    return 0;

}
