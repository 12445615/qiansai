#include <stdio.h>
#include <string.h>

#include "encoder.h"

int main(int argc, char **argv) {
    FFmpegStreamer s;
    const char *url;
    int rc;

    memset(&s, 0, sizeof(s));
    url = argc > 1 ? argv[1] : "rtmp://10.139.8.231:1935/fire_live/test";

    printf("[encoder_test] target=%s size=2112x1568 fps=60\n", url);
    fflush(stdout);

    rc = streamer_init(&s, url, 2112, 1568, 60);
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
