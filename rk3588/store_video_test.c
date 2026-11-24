#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "local_store.h"
#include "video_store.h"

int main(void) {
    LocalStore store;
    VideoStore video_store;
    char path[4096];
    int64_t segment_id = 0;
    int64_t bytes_after = 0;
    int deleted = 0;
    int rc;

    printf("=== store_video_test ===\n");
    fflush(stdout);

    memset(&store, 0, sizeof(store));
    memset(&video_store, 0, sizeof(video_store));

    rc = local_store_open(&store, NULL);
    printf("local_store_open rc=%d db=%s\n", rc, store.db_path);
    fflush(stdout);
    if (rc != 0) {
        return 1;
    }

    rc = video_store_init(&video_store, &store, NULL);
    printf("video_store_init rc=%d (%s)\n", rc, rc != 0 ? strerror(rc) : "ok");
    printf("  root_dir=%s\n  video_root=%s\n  segment_ms=%lld high=%lld low=%lld\n",
           video_store.root_dir,
           video_store.video_root,
           (long long)video_store.segment_duration_ms,
           (long long)video_store.high_water_bytes,
           (long long)video_store.low_water_bytes);
    fflush(stdout);
    if (rc != 0) {
        local_store_close(&store);
        return 2;
    }

    rc = video_store_build_segment_path(&video_store, 1700000000000LL, path, sizeof(path));
    printf("build_segment_path rc=%d path=%s\n", rc, rc == 0 ? path : "-");
    fflush(stdout);
    if (rc != 0) {
        local_store_close(&store);
        return 3;
    }

    {
        FILE *fp = fopen(path, "wb");
        if (fp != NULL) {
            fwrite("store-video-test", 1, 16, fp);
            fclose(fp);
        }
    }

    rc = video_store_begin_segment(&video_store, 1700000000000LL, path, &segment_id);
    printf("begin_segment rc=%d id=%lld\n", rc, (long long)segment_id);
    fflush(stdout);
    if (rc != 0) {
        local_store_close(&store);
        return 4;
    }

    rc = video_store_finish_segment(&video_store, segment_id, 1700000020000LL, 16);
    printf("finish_segment rc=%d\n", rc);
    fflush(stdout);

    rc = video_store_prune(&video_store, &bytes_after, &deleted);
    printf("prune rc=%d bytes_after=%lld deleted=%d\n", rc, (long long)bytes_after, deleted);
    fflush(stdout);

    local_store_close(&store);
    printf("=== done ===\n");
    return 0;
}
