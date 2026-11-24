#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "encoder.h"

int main(int argc, char **argv) {
    FFmpegStreamer s;
    const char *url;
    int width = argc > 2 ? atoi(argv[2]) : 1920;
    int height = argc > 3 ? atoi(argv[3]) : 1080;
    int fps = argc > 4 ? atoi(argv[4]) : 60;
    int rc;

    if (width <= 0 || height <= 0 || fps <= 0) {
        fprintf(stderr, "usage: %s [rtmp-url] [width] [height] [fps]\n", argv[0]);
        return 2;
    }

    memset(&s, 0, sizeof(s));
    url = argc > 1 ? argv[1] : "rtmp://192.168.1.101:1935/live/test";

    printf("[encoder_test] target=%s size=%dx%d fps=%d\n", url, width, height, fps);
    fflush(stdout);

    rc = streamer_init(&s, url, width, height, fps);
    printf("[encoder_test] streamer_init rc=%d\n", rc);
    printf("[encoder_test] fmt_ctx=%p video_st=%p codecpar=%p\n",
           (void *)s.fmt_ctx,
           (void *)s.video_st,
           s.video_st != NULL ? (void *)s.video_st->codecpar : NULL);
    fflush(stdout);

    if (rc == 0) {
        streamer_clean(&s);
        printf("[encoder_test] cleaned\n");
        fflush(stdout);
    }

    return rc != 0;
}
