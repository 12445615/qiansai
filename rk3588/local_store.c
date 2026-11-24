#define _POSIX_C_SOURCE 200809L

#include "local_store.h"

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// 本地离线队列是一个轻量 SQLite 单文件库，放在 SD 卡上做断网缓存。
#define LOCAL_STORE_DB_NAME "offline.db"
#define LOCAL_STORE_MAX_ROWS 100000
#define LOCAL_STORE_INIT_LOCK_NAME ".offline.db.init.lock"

static int local_store_copy_string(char *dst, size_t dst_size, const char *src) {
    if (dst == NULL || dst_size == 0) {
        return EINVAL;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return 0;
    }

    size_t len = strlen(src);
    if (len >= dst_size) {
        len = dst_size - 1;
    }

    memcpy(dst, src, len);
    dst[len] = '\0';
    return 0;
}

static int local_store_mkdirs(const char *path) {
    char tmp[PATH_MAX];
    size_t len;

    if (path == NULL || path[0] == '\0') {
        return EINVAL;
    }

    len = strlen(path);
    if (len >= sizeof(tmp)) {
        return ENAMETOOLONG;
    }

    memcpy(tmp, path, len + 1);
    if (len > 1 && tmp[len - 1] == '/') {
        tmp[len - 1] = '\0';
    }

    // 逐级创建目录，确保像 /mnt/sdcard/camera_flow 这种路径在首次启动时可用。
    for (char *p = tmp + 1; *p != '\0'; ++p) {
        if (*p != '/') {
            continue;
        }

        *p = '\0';
        if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
            return errno;
        }
        *p = '/';
    }

    if (mkdir(tmp, 0775) < 0 && errno != EEXIST) {
        return errno;
    }

    return 0;
}

static int local_store_open_init_lock(const char *root_dir, char *lock_path, size_t lock_path_size) {
    int fd;
    int rc;

    if (root_dir == NULL || lock_path == NULL || lock_path_size == 0) {
        return -1;
    }

    rc = snprintf(lock_path, lock_path_size, "%s/%s", root_dir, LOCAL_STORE_INIT_LOCK_NAME);
    if (rc < 0 || (size_t)rc >= lock_path_size) {
        errno = ENAMETOOLONG;
        return -1;
    }

    fd = open(lock_path, O_CREAT | O_RDWR, 0666);
    if (fd < 0) {
        return -1;
    }

    if (flock(fd, LOCK_EX) < 0) {
        close(fd);
        return -1;
    }

    return fd;
}

static void local_store_close_init_lock(int fd) {
    if (fd < 0) {
        return;
    }

    flock(fd, LOCK_UN);
    close(fd);
}

static int local_store_exec(LocalStore *store, const char *sql) {
    char *errmsg = NULL;
    int rc;

    rc = sqlite3_exec(store->db, sql, NULL, NULL, &errmsg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[LocalStore] sqlite exec failed: %s\n",
                errmsg != NULL ? errmsg : sqlite3_errmsg(store->db));
        sqlite3_free(errmsg);
        return EIO;
    }

    return 0;
}

static void local_store_fill_record(sqlite3_stmt *stmt, LocalStoreRecord *record) {
    const unsigned char *topic;
    const unsigned char *payload;
    const unsigned char *media_type;
    const unsigned char *media_path;

    record->id = sqlite3_column_int64(stmt, 0);
    record->created_at_ms = sqlite3_column_int64(stmt, 1);
    topic = sqlite3_column_text(stmt, 2);
    payload = sqlite3_column_text(stmt, 3);
    record->ppm = sqlite3_column_int(stmt, 4);
    record->temp = sqlite3_column_double(stmt, 5);
    record->humi = sqlite3_column_double(stmt, 6);
    record->alarm_status = sqlite3_column_int(stmt, 7);
    media_type = sqlite3_column_text(stmt, 8);
    media_path = sqlite3_column_text(stmt, 9);

    // sqlite3_column_text 返回的内存只在 stmt 生命周期内有效，这里需要拷贝出来。
    local_store_copy_string(record->topic, sizeof(record->topic), (const char *)topic);
    local_store_copy_string(record->payload, sizeof(record->payload), (const char *)payload);
    local_store_copy_string(record->media_type, sizeof(record->media_type), (const char *)media_type);
    local_store_copy_string(record->media_path, sizeof(record->media_path), (const char *)media_path);
}

static void local_store_fill_video_segment_record(sqlite3_stmt *stmt, LocalVideoSegmentRecord *record) {
    const unsigned char *file_path;
    const unsigned char *state;
    const unsigned char *last_error;
    const unsigned char *remote_path;

    record->id = sqlite3_column_int64(stmt, 0);
    record->start_ms = sqlite3_column_int64(stmt, 1);
    record->end_ms = sqlite3_column_int64(stmt, 2);
    record->size_bytes = sqlite3_column_int64(stmt, 3);
    record->retry_count = sqlite3_column_int(stmt, 4);
    record->uploaded_at_ms = sqlite3_column_int64(stmt, 5);
    state = sqlite3_column_text(stmt, 6);
    file_path = sqlite3_column_text(stmt, 7);
    record->created_at_ms = sqlite3_column_int64(stmt, 8);
    last_error = sqlite3_column_text(stmt, 9);
    remote_path = sqlite3_column_text(stmt, 10);

    local_store_copy_string(record->state, sizeof(record->state), (const char *)state);
    local_store_copy_string(record->file_path, sizeof(record->file_path), (const char *)file_path);
    local_store_copy_string(record->last_error, sizeof(record->last_error), (const char *)last_error);
    local_store_copy_string(record->remote_path, sizeof(record->remote_path), (const char *)remote_path);
}

static int64_t local_store_now_ms(void) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        return 0;
    }

    return (int64_t)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000LL;
}

static int local_store_debug_fetch_by_id(LocalStore *store,
                                         int64_t id,
                                         LocalStoreRecord *record,
                                         int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path "
        "FROM offline_queue WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_record(stmt, record);
        *found_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    *found_out = 0;
    return 0;
}

static int local_store_init_schema(LocalStore *store) {
    // payload 直接保存最终上报 JSON，网络恢复后可以原样补发，避免再次拼包。
    static const char *kSchemaSql =
        "CREATE TABLE IF NOT EXISTS offline_queue ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "created_at_ms INTEGER NOT NULL,"
        "topic TEXT NOT NULL,"
        "payload TEXT NOT NULL,"
        "ppm INTEGER NOT NULL,"
        "temp REAL NOT NULL,"
        "humi REAL NOT NULL,"
        "alarm_status INTEGER NOT NULL,"
        "media_type TEXT NOT NULL DEFAULT '',"
        "media_path TEXT NOT NULL DEFAULT ''"
        ");"
        "CREATE TABLE IF NOT EXISTS video_segments ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "start_ms INTEGER NOT NULL,"
        "end_ms INTEGER NOT NULL DEFAULT 0,"
        "size_bytes INTEGER NOT NULL DEFAULT 0,"
        "retry_count INTEGER NOT NULL DEFAULT 0,"
        "uploaded_at_ms INTEGER NOT NULL DEFAULT 0,"
        "state TEXT NOT NULL,"
        "file_path TEXT NOT NULL,"
        "last_error TEXT NOT NULL DEFAULT '',"
        "remote_path TEXT NOT NULL DEFAULT '',"
        "created_at_ms INTEGER NOT NULL"
        ");"
        "CREATE INDEX IF NOT EXISTS idx_offline_queue_created_at "
        "ON offline_queue(created_at_ms);"
        "CREATE INDEX IF NOT EXISTS idx_video_segments_start_ms "
        "ON video_segments(start_ms);"
        "CREATE INDEX IF NOT EXISTS idx_video_segments_state "
        "ON video_segments(state);";

    return local_store_exec(store, kSchemaSql);
}

static int local_store_column_exists(LocalStore *store,
                                     const char *table_name,
                                     const char *column_name,
                                     int *exists_out) {
    sqlite3_stmt *stmt = NULL;
    char sql[128];
    int rc;

    if (store == NULL || store->db == NULL || table_name == NULL || column_name == NULL || exists_out == NULL) {
        return EINVAL;
    }

    rc = snprintf(sql, sizeof(sql), "PRAGMA table_info(%s);", table_name);
    if (rc < 0 || (size_t)rc >= sizeof(sql)) {
        return ENAMETOOLONG;
    }

    rc = sqlite3_prepare_v2(store->db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    *exists_out = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const unsigned char *name = sqlite3_column_text(stmt, 1);
        if (name != NULL && strcmp((const char *)name, column_name) == 0) {
            *exists_out = 1;
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

    return 0;
}

static int local_store_ensure_video_segments_columns(LocalStore *store) {
    struct ColumnPatch {
        const char *name;
        const char *sql;
    };
    static const struct ColumnPatch patches[] = {
        { "retry_count", "ALTER TABLE video_segments ADD COLUMN retry_count INTEGER NOT NULL DEFAULT 0;" },
        { "uploaded_at_ms", "ALTER TABLE video_segments ADD COLUMN uploaded_at_ms INTEGER NOT NULL DEFAULT 0;" },
        { "last_error", "ALTER TABLE video_segments ADD COLUMN last_error TEXT NOT NULL DEFAULT '';" },
        { "remote_path", "ALTER TABLE video_segments ADD COLUMN remote_path TEXT NOT NULL DEFAULT '';" },
    };
    int exists = 0;
    int exists_after = 0;
    int rc;

    for (size_t i = 0; i < sizeof(patches) / sizeof(patches[0]); ++i) {
        rc = local_store_column_exists(store, "video_segments", patches[i].name, &exists);
        if (rc != 0) {
            return rc;
        }
        if (!exists) {
            rc = local_store_exec(store, patches[i].sql);
            if (rc != 0) {
                // 多进程/多线程同时启动时，另一方可能刚好先一步把列补上。
                // 这里复查一次当前列是否已经存在，存在就把这次迁移视为成功。
                rc = local_store_column_exists(store, "video_segments", patches[i].name, &exists_after);
                if (rc != 0) {
                    return rc;
                }
                if (!exists_after) {
                    return EIO;
                }
            }
        }
    }

    return 0;
}

static int local_store_prune(LocalStore *store) {
    sqlite3_stmt *count_stmt = NULL;
    sqlite3_stmt *delete_stmt = NULL;
    int rc;
    int count = 0;

    rc = sqlite3_prepare_v2(store->db, "SELECT COUNT(*) FROM offline_queue;", -1, &count_stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    if (sqlite3_step(count_stmt) == SQLITE_ROW) {
        count = sqlite3_column_int(count_stmt, 0);
    }
    sqlite3_finalize(count_stmt);

    if (count <= store->max_rows) {
        return 0;
    }

    // 超出上限时删除最旧的数据，防止 SD 卡被持续离线写满。
    rc = sqlite3_prepare_v2(
        store->db,
        "DELETE FROM offline_queue WHERE id IN ("
        "SELECT id FROM offline_queue ORDER BY id ASC LIMIT ?"
        ");",
        -1,
        &delete_stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int(delete_stmt, 1, count - store->max_rows);
    rc = sqlite3_step(delete_stmt);
    sqlite3_finalize(delete_stmt);

    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}


/* ------------------------------------------------------------------------- */
/* 存储根目录解析                                                             */
/*                                                                            */
/* 历史问题：默认根目录写死成 SD 卡自动挂载点（/media/elf/<UUID>/camera_flow）。  */
/* 卡没插时该路径是一个「悬空软链」，mkdir 返回 ENOENT，local_store_open 直接   */
/* 失败，调用方只看到一句 local_store_open failed，既没有路径也没有原因。        */
/* 现在改为按候选列表逐个探测，并真的写一次探针文件。                            */
/* ------------------------------------------------------------------------- */

#define LOCAL_STORE_PROBE_FILE_NAME ".store_write_probe"
#define LOCAL_STORE_MIN_FREE_BYTES (256LL * 1024 * 1024)
#define LOCAL_STORE_MIN_FREE_BYTES_TMP (64LL * 1024 * 1024)
#define LOCAL_STORE_MAX_MOUNT_CANDIDATES 8

static char g_store_root[PATH_MAX];
static int g_store_root_rc = ENODEV;
static char g_store_root_reason[512];
static pthread_once_t g_store_root_once = PTHREAD_ONCE_INIT;

static void local_store_store_result(int rc, const char *path, const char *reason) {
    g_store_root_rc = rc;

    if (rc == 0 && path != NULL) {
        snprintf(g_store_root, sizeof(g_store_root), "%s", path);
    }
    snprintf(g_store_root_reason, sizeof(g_store_root_reason), "%s", reason != NULL ? reason : "");
}

/*
 * 判定候选目录是否真的可用：建目录 → statvfs（只读/剩余空间）→ 写探针文件并 fsync。
 * 返回 0 表示可用，并把剩余字节数写进 free_out。
 */
static int local_store_probe_root(const char *path, int64_t min_free_bytes, int64_t *free_out) {
    char probe_path[PATH_MAX];
    struct statvfs vfs;
    int64_t free_bytes;
    int fd;
    int rc;
    ssize_t written;
    int write_err = 0;

    if (free_out != NULL) {
        *free_out = 0;
    }
    if (path == NULL || path[0] != '/') {
        return EINVAL;
    }

    rc = local_store_mkdirs(path);
    if (rc != 0) {
        return rc;
    }

    if (statvfs(path, &vfs) < 0) {
        return errno;
    }
    if ((vfs.f_flag & ST_RDONLY) != 0) {
        return EROFS;
    }

    free_bytes = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
    if (free_out != NULL) {
        *free_out = free_bytes;
    }
    if (min_free_bytes > 0 && free_bytes < min_free_bytes) {
        return ENOSPC;
    }

    rc = snprintf(probe_path, sizeof(probe_path), "%s/%s", path, LOCAL_STORE_PROBE_FILE_NAME);
    if (rc < 0 || (size_t)rc >= sizeof(probe_path)) {
        return ENAMETOOLONG;
    }

    fd = open(probe_path, O_CREAT | O_WRONLY | O_TRUNC, 0666);
    if (fd < 0) {
        return errno;
    }

    written = write(fd, "ok\n", 3);
    if (written != 3) {
        write_err = written < 0 ? errno : EIO;
    }
    if (fsync(fd) < 0 && write_err == 0) {
        write_err = errno;
    }
    if (close(fd) < 0 && write_err == 0) {
        write_err = errno;
    }
    unlink(probe_path);

    return write_err;
}

/* /proc/mounts 里的空格等字符是八进制转义的，这里还原成真实路径。 */
static void local_store_unescape_mount_field(char *field) {
    char *src = field;
    char *dst = field;

    if (field == NULL) {
        return;
    }

    while (*src != '\0') {
        if (*src == '\\' && src[1] >= '0' && src[1] <= '7' && src[2] >= '0' && src[2] <= '7' &&
            src[3] >= '0' && src[3] <= '7') {
            *dst++ = (char)(((src[1] - '0') << 6) | ((src[2] - '0') << 3) | (src[3] - '0'));
            src += 4;
            continue;
        }
        *dst++ = *src++;
    }
    *dst = '\0';
}

/* 拼 "<base>/<sub>"，显式做长度检查，避免 snprintf 截断告警。 */
static int local_store_join_subdir(char *out, size_t out_size, const char *base, const char *sub) {
    size_t base_len;
    size_t sub_len;

    if (out == NULL || out_size == 0 || base == NULL || sub == NULL) {
        return EINVAL;
    }

    base_len = strlen(base);
    sub_len = strlen(sub);
    if (base_len == 0 || base_len + sub_len + 2 > out_size) {
        return ENAMETOOLONG;
    }

    memcpy(out, base, base_len);
    out[base_len] = '/';
    memcpy(out + base_len + 1, sub, sub_len + 1);
    return 0;
}

static int local_store_mount_is_removable(const char *mountpoint) {
    if (mountpoint == NULL) {
        return 0;
    }
    if (strncmp(mountpoint, "/media/", 7) == 0 && mountpoint[7] != '\0') {
        return 1;
    }
    if (strncmp(mountpoint, "/run/media/", 11) == 0 && mountpoint[11] != '\0') {
        return 1;
    }
    return 0;
}

/* 枚举 /media、/run/media 下的真实挂载点，跳过虚拟文件系统。 */
static int local_store_collect_mount_candidates(char candidates[][PATH_MAX], int max_candidates) {
    static const char *const kSkipFs[] = {
        "tmpfs", "devtmpfs", "proc", "sysfs", "cgroup", "cgroup2", "debugfs", "tracefs",
        "overlay", "squashfs", "ramfs", "configfs", "fusectl", "securityfs", "pstore",
        "bpf", "mqueue", "hugetlbfs", "autofs", "nsfs", "binfmt_misc", "devpts",
    };
    char device[PATH_MAX];
    char mountpoint[PATH_MAX];
    char fstype[64];
    char options[512];
    FILE *fp;
    int count = 0;
    int skip;
    size_t i;

    if (candidates == NULL || max_candidates <= 0) {
        return 0;
    }

    fp = fopen("/proc/mounts", "r");
    if (fp == NULL) {
        return 0;
    }

    while (count < max_candidates &&
           fscanf(fp, "%4095s %4095s %63s %511s%*[^\n]", device, mountpoint, fstype, options) == 4) {
        local_store_unescape_mount_field(mountpoint);
        if (!local_store_mount_is_removable(mountpoint)) {
            continue;
        }

        skip = 0;
        for (i = 0; i < sizeof(kSkipFs) / sizeof(kSkipFs[0]); ++i) {
            if (strcmp(fstype, kSkipFs[i]) == 0) {
                skip = 1;
                break;
            }
        }
        if (skip) {
            continue;
        }

        snprintf(candidates[count], PATH_MAX, "%s", mountpoint);
        count++;
    }

    fclose(fp);
    return count;
}

static void local_store_resolve_root_impl(void) {
    /* 只列真实存在的板载数据分区：/data 在本镜像上并不存在，探测时会被 mkdir 造出来，别污染根分区。 */
    static const char *const kLocalRoots[] = { "/userdata", "/var/lib" };
    char candidates[LOCAL_STORE_MAX_MOUNT_CANDIDATES][PATH_MAX];
    char path[PATH_MAX];
    char best[PATH_MAX];
    char reason[512];
    const char *env_root;
    int64_t free_bytes = 0;
    int64_t best_free = 0;
    int64_t chosen_free = 0;
    int have_best = 0;
    int count;
    int rc;
    int i;
    size_t k;

    memset(best, 0, sizeof(best));
    snprintf(reason, sizeof(reason), "no writable candidate found");

    /* 1) 显式指定优先：指定了却没生效要留下明确日志，然后继续自动降级，而不是让整机功能失效。 */
    env_root = getenv("CAMERA_FLOW_STORE_DIR");
    if (env_root != NULL && env_root[0] != '\0') {
        rc = local_store_probe_root(env_root, LOCAL_STORE_MIN_FREE_BYTES, &free_bytes);
        if (rc == 0) {
            local_store_store_result(0, env_root, "CAMERA_FLOW_STORE_DIR");
            chosen_free = free_bytes;
            goto done;
        }
        fprintf(stderr,
                "[LocalStore] CAMERA_FLOW_STORE_DIR=%s unusable: %s (%d), falling back to auto-detect\n",
                env_root,
                strerror(rc),
                rc);
        snprintf(reason, sizeof(reason), "CAMERA_FLOW_STORE_DIR=%s unusable: %s", env_root, strerror(rc));
    }

    /* 2) SD 卡 / U 盘：多张卡时按剩余空间择优。 */
    count = local_store_collect_mount_candidates(candidates, LOCAL_STORE_MAX_MOUNT_CANDIDATES);
    for (i = 0; i < count; ++i) {
        rc = local_store_join_subdir(path, sizeof(path), candidates[i], "camera_flow");
        if (rc != 0) {
            continue;
        }
        rc = local_store_probe_root(path, LOCAL_STORE_MIN_FREE_BYTES, &free_bytes);
        if (rc != 0) {
            fprintf(stderr, "[LocalStore] removable candidate %s unusable: %s (%d)\n", path, strerror(rc), rc);
            continue;
        }
        if (!have_best || free_bytes > best_free) {
            snprintf(best, sizeof(best), "%s", path);
            best_free = free_bytes;
            have_best = 1;
        }
    }
    if (have_best) {
        local_store_store_result(0, best, "removable mount");
        chosen_free = best_free;
        goto done;
    }
    snprintf(reason, sizeof(reason), "no writable removable mount among %d candidate(s)", count);

    /* 3) 板载数据分区：/userdata 往往只有几百 MB，所以也按剩余空间择优。 */
    for (k = 0; k < sizeof(kLocalRoots) / sizeof(kLocalRoots[0]); ++k) {
        rc = local_store_join_subdir(path, sizeof(path), kLocalRoots[k], "camera_flow");
        if (rc != 0) {
            continue;
        }
        rc = local_store_probe_root(path, LOCAL_STORE_MIN_FREE_BYTES, &free_bytes);
        if (rc != 0) {
            fprintf(stderr, "[LocalStore] onboard candidate %s unusable: %s (%d)\n", path, strerror(rc), rc);
            continue;
        }
        if (!have_best || free_bytes > best_free) {
            snprintf(best, sizeof(best), "%s", path);
            best_free = free_bytes;
            have_best = 1;
        }
    }
    if (have_best) {
        local_store_store_result(0, best, "onboard data partition");
        chosen_free = best_free;
        goto done;
    }

    /* 4) 最后兜底：/tmp 一定能用，但重启即丢，必须显式告警。 */
    rc = local_store_probe_root("/tmp/camera_flow", LOCAL_STORE_MIN_FREE_BYTES_TMP, &free_bytes);
    if (rc == 0) {
        fprintf(stderr,
                "[LocalStore] WARNING: falling back to volatile /tmp/camera_flow, cached data is lost on reboot\n");
        local_store_store_result(0, "/tmp/camera_flow", "volatile /tmp fallback");
        chosen_free = free_bytes;
        goto done;
    }

    local_store_store_result(rc, NULL, reason);
    fprintf(stderr, "[LocalStore] no usable store root: %s (%s)\n", reason, strerror(rc));
    return;

done:
    fprintf(stderr,
            "[LocalStore] root resolved: %s (source: %s, free: %lld MB)\n",
            g_store_root,
            g_store_root_reason,
            (long long)(chosen_free / (1024 * 1024)));
}

const char *local_store_resolve_root_cached(void) {
    pthread_once(&g_store_root_once, local_store_resolve_root_impl);
    return g_store_root_rc == 0 ? g_store_root : NULL;
}

int local_store_resolve_root(char *out, size_t out_size) {
    const char *root;
    size_t len;

    if (out == NULL || out_size == 0) {
        return EINVAL;
    }

    root = local_store_resolve_root_cached();
    if (root == NULL) {
        return g_store_root_rc != 0 ? g_store_root_rc : ENODEV;
    }

    len = strlen(root);
    if (len + 1 > out_size) {
        return ENAMETOOLONG;
    }

    memcpy(out, root, len + 1);
    return 0;
}

int local_store_open(LocalStore *store, const char *root_dir) {
    char resolved_root[PATH_MAX];
    const char *effective_root = root_dir;
    char lock_path[PATH_MAX];
    int lock_fd = -1;
    int rc;

    if (store == NULL) {
        return EINVAL;
    }

    memset(store, 0, sizeof(*store));
    store->max_rows = LOCAL_STORE_MAX_ROWS;

    /* 显式传入的根目录同样先探测：不可写作废并回退自动解析，而不是让整机失去缓存能力。 */
    if (effective_root != NULL && effective_root[0] != '\0') {
        rc = local_store_probe_root(effective_root, LOCAL_STORE_MIN_FREE_BYTES_TMP, NULL);
        if (rc != 0) {
            fprintf(stderr,
                    "[LocalStore] requested root %s unusable: %s (%d), falling back to auto-detect\n",
                    effective_root,
                    strerror(rc),
                    rc);
            effective_root = NULL;
        }
    }

    if (effective_root == NULL || effective_root[0] == '\0') {
        rc = local_store_resolve_root(resolved_root, sizeof(resolved_root));
        if (rc != 0) {
            fprintf(stderr, "[LocalStore] no usable store root: %s (%d)\n", strerror(rc), rc);
            return rc;
        }
        effective_root = resolved_root;
    }

    rc = local_store_mkdirs(effective_root);
    if (rc != 0) {
        fprintf(stderr,
                "[LocalStore] cannot create store root %s: %s (%d)\n",
                effective_root,
                strerror(rc),
                rc);
        return rc;
    }

    rc = snprintf(store->db_path, sizeof(store->db_path), "%s/%s", effective_root, LOCAL_STORE_DB_NAME);
    if (rc < 0 || (size_t)rc >= sizeof(store->db_path)) {
        return ENAMETOOLONG;
    }

    lock_fd = local_store_open_init_lock(effective_root, lock_path, sizeof(lock_path));
    if (lock_fd < 0) {
        rc = errno != 0 ? errno : EIO;
        fprintf(stderr, "[LocalStore] cannot open init lock under %s: %s (%d)\n",
                effective_root,
                strerror(rc),
                rc);
        return rc;
    }

    rc = sqlite3_open(store->db_path, &store->db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "[LocalStore] sqlite open failed for %s: %s\n",
                store->db_path,
                store->db != NULL ? sqlite3_errmsg(store->db) : "unknown");
        local_store_close(store);
        rc = EIO;
        goto unlock_and_exit;
    }

    sqlite3_busy_timeout(store->db, 1000);

    // WAL + NORMAL 在 SD 卡场景下比默认模式更适合频繁写入和异常恢复。
    rc = local_store_exec(store, "PRAGMA journal_mode=WAL;");
    if (rc != 0) {
        fprintf(stderr, "[LocalStore] WAL pragma failed on %s\n", store->db_path);
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_exec(store, "PRAGMA synchronous=NORMAL;");
    if (rc != 0) {
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_init_schema(store);
    if (rc != 0) {
        fprintf(stderr, "[LocalStore] schema init failed on %s\n", store->db_path);
        local_store_close(store);
        goto unlock_and_exit;
    }

    rc = local_store_ensure_video_segments_columns(store);
    if (rc != 0) {
        fprintf(stderr, "[LocalStore] video_segments migration failed on %s\n", store->db_path);
        local_store_close(store);
        goto unlock_and_exit;
    }

unlock_and_exit:
    local_store_close_init_lock(lock_fd);
    if (rc == 0) {
        printf("[LocalStore] ready: %s\n", store->db_path);
        fflush(stdout);
    }
    return rc;
}

void local_store_close(LocalStore *store) {
    if (store == NULL) {
        return;
    }

    if (store->db != NULL) {
        sqlite3_close(store->db);
        store->db = NULL;
    }
}

int local_store_enqueue(LocalStore *store,
                        int64_t created_at_ms,
                        const char *topic,
                        const char *payload,
                        int ppm,
                        double temp,
                        double humi,
                        int alarm_status,
                        const char *media_type,
                        const char *media_path) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || topic == NULL || payload == NULL) {
        return EINVAL;
    }

    // 结构化字段和完整 payload 一起落库，后面既能查业务字段，也能直接补发。
    rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO offline_queue ("
        "created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path"
        ") VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?);",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, created_at_ms);
    sqlite3_bind_text(stmt, 2, topic, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, payload, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(stmt, 4, ppm);
    sqlite3_bind_double(stmt, 5, temp);
    sqlite3_bind_double(stmt, 6, humi);
    sqlite3_bind_int(stmt, 7, alarm_status);
    sqlite3_bind_text(stmt, 8, media_type != NULL ? media_type : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 9, media_path != NULL ? media_path : "", -1, SQLITE_TRANSIENT);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return local_store_prune(store);
}

int local_store_fetch_batch(LocalStore *store,
                            LocalStoreRecord *records,
                            int max_records,
                            int *count_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;

    if (store == NULL || store->db == NULL || records == NULL || max_records <= 0 || count_out == NULL) {
        return EINVAL;
    }

    // 按自增 id 顺序取最旧的记录，保证补发顺序和离线写入顺序一致。
    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, created_at_ms, topic, payload, ppm, temp, humi, alarm_status, media_type, media_path "
        "FROM offline_queue ORDER BY id ASC LIMIT ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int(stmt, 1, max_records);

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        local_store_fill_record(stmt, &records[count]);
        count++;
        if (count >= max_records) {
            break;
        }
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

    *count_out = count;
    return 0;
}

int local_store_delete(LocalStore *store, int64_t id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "DELETE FROM offline_queue WHERE id = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_pending_count(LocalStore *store, int *count_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || count_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "SELECT COUNT(*) FROM offline_queue;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return EIO;
    }

    *count_out = sqlite3_column_int(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int local_store_register_video_segment(LocalStore *store,
                                       int64_t start_ms,
                                       const char *file_path,
                                       const char *state,
                                       int64_t *segment_id_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || file_path == NULL || state == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "INSERT INTO video_segments (start_ms, state, file_path, created_at_ms) "
        "VALUES (?, ?, ?, ?);",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, start_ms);
    sqlite3_bind_text(stmt, 2, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(stmt, 3, file_path, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, local_store_now_ms());

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    if (segment_id_out != NULL) {
        *segment_id_out = sqlite3_last_insert_rowid(store->db);
    }

    return 0;
}

int local_store_update_video_segment(LocalStore *store,
                                     int64_t segment_id,
                                     int64_t end_ms,
                                     int64_t size_bytes,
                                     const char *state) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || state == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET end_ms = ?, size_bytes = ?, state = ? WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, end_ms);
    sqlite3_bind_int64(stmt, 2, size_bytes);
    sqlite3_bind_text(stmt, 3, state, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 4, segment_id);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_delete_video_segment(LocalStore *store, int64_t segment_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(store->db, "DELETE FROM video_segments WHERE id = ?;", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_fetch_oldest_prunable_video_segment(LocalStore *store,
                                                    LocalVideoSegmentRecord *record,
                                                    int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments "
        "WHERE state IN ('uploaded', 'pending', 'broken') "
        "ORDER BY retry_count ASC, start_ms ASC LIMIT 1;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
        *found_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    *found_out = 0;
    return 0;
}

int local_store_fetch_oldest_pending_video_segment(LocalStore *store,
                                                   LocalVideoSegmentRecord *record,
                                                   int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments "
        "WHERE state = 'pending' "
        "ORDER BY retry_count ASC, start_ms ASC LIMIT 1;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
        *found_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    *found_out = 0;
    return 0;
}

int local_store_video_total_size(LocalStore *store, int64_t *bytes_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || bytes_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT COALESCE(SUM(size_bytes), 0) FROM video_segments "
        "WHERE state IN ('recording', 'pending', 'uploaded', 'broken');",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        return EIO;
    }

    *bytes_out = sqlite3_column_int64(stmt, 0);
    sqlite3_finalize(stmt);
    return 0;
}

int local_store_mark_video_segment_uploading(LocalStore *store, int64_t segment_id) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET state = 'uploading' WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_mark_video_segment_uploaded(LocalStore *store,
                                            int64_t segment_id,
                                            const char *remote_path,
                                            int64_t uploaded_at_ms) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments "
        "SET state = 'uploaded', uploaded_at_ms = ?, remote_path = ?, last_error = '' "
        "WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, uploaded_at_ms);
    sqlite3_bind_text(stmt, 2, remote_path != NULL ? remote_path : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 3, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_mark_video_segment_retry(LocalStore *store,
                                         int64_t segment_id,
                                         const char *last_error) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments "
        "SET state = 'pending', retry_count = retry_count + 1, last_error = ? "
        "WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_text(stmt, 1, last_error != NULL ? last_error : "", -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(stmt, 2, segment_id);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}

int local_store_reset_uploading_video_segments(LocalStore *store) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET state = 'pending' WHERE state = 'uploading';",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    return 0;
}


int local_store_recover_recording_video_segments(LocalStore *store) {
    sqlite3_stmt *select_stmt = NULL;
    sqlite3_stmt *update_stmt = NULL;
    int rc;
    int recovered = 0;
    int broken = 0;

    if (store == NULL || store->db == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, file_path FROM video_segments WHERE state = 'recording';",
        -1,
        &select_stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "UPDATE video_segments SET state = ?, end_ms = CASE WHEN end_ms = 0 THEN created_at_ms ELSE end_ms END, size_bytes = ?, last_error = ? WHERE id = ?;",
        -1,
        &update_stmt,
        NULL);
    if (rc != SQLITE_OK) {
        sqlite3_finalize(select_stmt);
        return EIO;
    }

    while ((rc = sqlite3_step(select_stmt)) == SQLITE_ROW) {
        int64_t id = sqlite3_column_int64(select_stmt, 0);
        const unsigned char *path_text = sqlite3_column_text(select_stmt, 1);
        const char *path = path_text != NULL ? (const char *)path_text : "";
        struct stat st;
        const char *state = "broken";
        const char *error = "recording segment recovered without data";
        int64_t size = 0;

        if (path[0] != '\0' && stat(path, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) {
            state = "pending";
            error = "";
            size = (int64_t)st.st_size;
            recovered++;
        } else {
            broken++;
        }

        sqlite3_reset(update_stmt);
        sqlite3_clear_bindings(update_stmt);
        sqlite3_bind_text(update_stmt, 1, state, -1, SQLITE_STATIC);
        sqlite3_bind_int64(update_stmt, 2, size);
        sqlite3_bind_text(update_stmt, 3, error, -1, SQLITE_STATIC);
        sqlite3_bind_int64(update_stmt, 4, id);
        if (sqlite3_step(update_stmt) != SQLITE_DONE) {
            sqlite3_finalize(update_stmt);
            sqlite3_finalize(select_stmt);
            return EIO;
        }
    }

    sqlite3_finalize(update_stmt);
    sqlite3_finalize(select_stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    if (recovered > 0 || broken > 0) {
        printf("[VideoStore] recovered recording segments: pending=%d broken=%d\n", recovered, broken);
    }
    return 0;
}

int local_store_debug_fetch_video_segment_by_id(LocalStore *store,
                                                int64_t id,
                                                LocalVideoSegmentRecord *record,
                                                int *found_out) {
    sqlite3_stmt *stmt = NULL;
    int rc;

    if (store == NULL || store->db == NULL || record == NULL || found_out == NULL) {
        return EINVAL;
    }

    rc = sqlite3_prepare_v2(
        store->db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments WHERE id = ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        return EIO;
    }

    sqlite3_bind_int64(stmt, 1, id);
    rc = sqlite3_step(stmt);
    if (rc == SQLITE_ROW) {
        local_store_fill_video_segment_record(stmt, record);
        *found_out = 1;
        sqlite3_finalize(stmt);
        return 0;
    }

    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) {
        return EIO;
    }

    *found_out = 0;
    return 0;
}

int local_store_debug_dump_video_segments(const char *root_dir, int limit) {
    LocalStore store;
    sqlite3_stmt *stmt = NULL;
    int rc;
    int count = 0;

    if (limit <= 0) {
        limit = 10;
    }

    memset(&store, 0, sizeof(store));
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        return rc;
    }

    rc = sqlite3_prepare_v2(
        store.db,
        "SELECT id, start_ms, end_ms, size_bytes, retry_count, uploaded_at_ms, "
        "state, file_path, created_at_ms, last_error, remote_path "
        "FROM video_segments ORDER BY id DESC LIMIT ?;",
        -1,
        &stmt,
        NULL);
    if (rc != SQLITE_OK) {
        local_store_close(&store);
        return EIO;
    }

    sqlite3_bind_int(stmt, 1, limit);
    printf("[LocalStoreVideoDump] db path: %s\n", store.db_path);
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        LocalVideoSegmentRecord record;
        memset(&record, 0, sizeof(record));
        local_store_fill_video_segment_record(stmt, &record);
        printf("[LocalStoreVideoDump] row[%d] id=%lld state=%s retry=%d size=%lld path=%s remote=%s err=%s\n",
               count,
               (long long)record.id,
               record.state,
               record.retry_count,
               (long long)record.size_bytes,
               record.file_path,
               record.remote_path,
               record.last_error);
        count++;
    }

    sqlite3_finalize(stmt);
    local_store_close(&store);
    if (rc != SQLITE_DONE && rc != SQLITE_ROW) {
        return EIO;
    }

    return 0;
}

int local_store_debug_selftest(const char *root_dir) {
    LocalStore store;
    LocalStoreRecord record;
    int rc;
    int pending_before = 0;
    int pending_after_enqueue = 0;
    int pending_after_delete = 0;
    int found = 0;
    int64_t inserted_id;
    int64_t test_ts = local_store_now_ms();
    char payload[LOCAL_STORE_MAX_PAYLOAD_LEN];

    memset(&store, 0, sizeof(store));
    memset(&record, 0, sizeof(record));

    printf("[LocalStoreTest] opening store...\n");
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        printf("[LocalStoreTest] open failed: %d\n", rc);
        return rc;
    }

    printf("[LocalStoreTest] db path: %s\n", store.db_path);

    rc = local_store_pending_count(&store, &pending_before);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(before) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending before: %d\n", pending_before);

    rc = snprintf(payload,
                  sizeof(payload),
                  "{\"debug\":true,\"ts\":%lld,\"source\":\"local_store_selftest\"}",
                  (long long)test_ts);
    if (rc < 0 || (size_t)rc >= sizeof(payload)) {
        local_store_close(&store);
        return ENOSPC;
    }

    rc = local_store_enqueue(&store,
                             test_ts,
                             "/debug/local_store",
                             payload,
                             123,
                             25.5,
                             60.0,
                             0,
                             "none",
                             "");
    if (rc != 0) {
        printf("[LocalStoreTest] enqueue failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    inserted_id = sqlite3_last_insert_rowid(store.db);
    printf("[LocalStoreTest] enqueue ok, id=%lld\n", (long long)inserted_id);

    rc = local_store_pending_count(&store, &pending_after_enqueue);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(after enqueue) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending after enqueue: %d\n", pending_after_enqueue);

    rc = local_store_debug_fetch_by_id(&store, inserted_id, &record, &found);
    if (rc != 0) {
        printf("[LocalStoreTest] fetch_by_id failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    if (!found) {
        printf("[LocalStoreTest] inserted row not found: id=%lld\n", (long long)inserted_id);
        local_store_close(&store);
        return ENOENT;
    }

    printf("[LocalStoreTest] fetched row: id=%lld ts=%lld ppm=%d temp=%.1f humi=%.1f alarm=%d\n",
           (long long)record.id,
           (long long)record.created_at_ms,
           record.ppm,
           record.temp,
           record.humi,
           record.alarm_status);
    printf("[LocalStoreTest] topic=%s\n", record.topic);
    printf("[LocalStoreTest] payload=%s\n", record.payload);

    rc = local_store_delete(&store, inserted_id);
    if (rc != 0) {
        printf("[LocalStoreTest] delete failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] delete ok: id=%lld\n", (long long)inserted_id);

    rc = local_store_pending_count(&store, &pending_after_delete);
    if (rc != 0) {
        printf("[LocalStoreTest] pending_count(after delete) failed: %d\n", rc);
        local_store_close(&store);
        return rc;
    }
    printf("[LocalStoreTest] pending after delete: %d\n", pending_after_delete);

    local_store_close(&store);
    printf("[LocalStoreTest] selftest done\n");
    return 0;
}

int local_store_debug_dump(const char *root_dir, int limit) {
    LocalStore store;
    LocalStoreRecord *records = NULL;
    int rc;
    int count = 0;
    int pending = 0;

    if (limit <= 0) {
        limit = 10;
    }

    records = calloc((size_t)limit, sizeof(*records));
    if (records == NULL) {
        return ENOMEM;
    }

    memset(&store, 0, sizeof(store));
    rc = local_store_open(&store, root_dir);
    if (rc != 0) {
        free(records);
        return rc;
    }

    rc = local_store_pending_count(&store, &pending);
    if (rc != 0) {
        printf("[LocalStoreDump] pending_count failed: %d\n", rc);
        local_store_close(&store);
        free(records);
        return rc;
    }

    rc = local_store_fetch_batch(&store, records, limit, &count);
    if (rc != 0) {
        printf("[LocalStoreDump] fetch failed: %d\n", rc);
        local_store_close(&store);
        free(records);
        return rc;
    }

    printf("[LocalStoreDump] db path: %s\n", store.db_path);
    printf("[LocalStoreDump] pending rows: %d, fetched: %d\n", pending, count);
    for (int i = 0; i < count; ++i) {
        printf("[LocalStoreDump] row[%d] id=%lld ts=%lld ppm=%d temp=%.1f humi=%.1f alarm=%d topic=%s\n",
               i,
               (long long)records[i].id,
               (long long)records[i].created_at_ms,
               records[i].ppm,
               records[i].temp,
               records[i].humi,
               records[i].alarm_status,
               records[i].topic);
        printf("[LocalStoreDump] row[%d] payload=%s\n", i, records[i].payload);
    }

    local_store_close(&store);
    free(records);
    return 0;
}
