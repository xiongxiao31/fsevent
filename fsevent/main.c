// main.c
// Compile (example):
// clang main.c -framework CoreServices -framework CoreFoundation -lsqlite3 -lpthread -lleveldb -o fsevent_vcs
//
// 注意：根据你的系统 link flags 和 leveldb 安装方式可能需调整 -lleveldb 路径/选项

#include <CoreServices/CoreServices.h>
#include <sqlite3.h>
#include <CommonCrypto/CommonDigest.h>
#include <mach/mach_time.h>
#include <ftw.h>
#include <dispatch/dispatch.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <time.h>
#include <fnmatch.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
#include <limits.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include "leveldb/c.h"
#include "vbproto_pb.h"
#include "pb.h"
#include "uthash.h"
#include "RepoMap.h"

#define WORKSPACE "/Users/jexyxiong/testfsevent"

#define WORKER_QUEUE_LABEL "com.example.fsevent.worker"
#define HTTP_PORT 8079
#define LISTEN_BACKLOG 32


typedef struct {
    char *path; // strdup'd
    char *root;
    int64_t flag;
    uint64 eventid;
} Job;
int64_t startUpTime;
leveldb_t *db;
dispatch_queue_t work_q;

typedef struct RestartWatcher {
    char path[PATH_MAX];
    dispatch_source_t timer;
    FSEventStreamEventId resume_from;
    UT_hash_handle hh;
} RestartWatcher;

static RestartWatcher *g_restart_watchers = NULL;
static pthread_mutex_t g_restart_lock = PTHREAD_MUTEX_INITIALIZER;
/* ---------- utility functions (kept from your original) ---------- */

void print_fsevent_flags(FSEventStreamEventFlags flags) {
    if (flags == kFSEventStreamEventFlagNone) printf(" - None\n");
    if (flags & kFSEventStreamEventFlagMustScanSubDirs) printf(" - MustScanSubDirs\n");
    if (flags & kFSEventStreamEventFlagUserDropped) printf(" - UserDropped\n");
    if (flags & kFSEventStreamEventFlagKernelDropped) printf(" - KernelDropped\n");
    if (flags & kFSEventStreamEventFlagEventIdsWrapped) printf(" - EventIdsWrapped\n");
    if (flags & kFSEventStreamEventFlagHistoryDone) printf(" - HistoryDone\n");
    if (flags & kFSEventStreamEventFlagRootChanged) printf(" - RootChanged\n");
    if (flags & kFSEventStreamEventFlagMount) printf(" - Mount\n");
    if (flags & kFSEventStreamEventFlagUnmount) printf(" - Unmount\n");
    if (flags & kFSEventStreamEventFlagItemCreated) printf(" - ItemCreated\n");
    if (flags & kFSEventStreamEventFlagItemRemoved) printf(" - ItemRemoved\n");
    if (flags & kFSEventStreamEventFlagItemInodeMetaMod) printf(" - ItemInodeMetaMod\n");
    if (flags & kFSEventStreamEventFlagItemRenamed) printf(" - ItemRenamed\n");
    if (flags & kFSEventStreamEventFlagItemModified) printf(" - ItemModified\n");
    if (flags & kFSEventStreamEventFlagItemFinderInfoMod) printf(" - ItemFinderInfoMod\n");
    if (flags & kFSEventStreamEventFlagItemChangeOwner) printf(" - ItemChangeOwner\n");
    if (flags & kFSEventStreamEventFlagItemXattrMod) printf(" - ItemXattrMod\n");
    if (flags & kFSEventStreamEventFlagItemIsFile) printf(" - ItemIsFile\n");
    if (flags & kFSEventStreamEventFlagItemIsDir) printf(" - ItemIsDir\n");
    if (flags & kFSEventStreamEventFlagItemIsSymlink) printf(" - ItemIsSymlink\n");
    if (flags & kFSEventStreamEventFlagOwnEvent) printf(" - OwnEvent\n");
    if (flags & kFSEventStreamEventFlagItemIsHardlink) printf(" - ItemIsHardlink\n");
    if (flags & kFSEventStreamEventFlagItemIsLastHardlink) printf(" - ItemIsLastHardlink\n");
}

/* basename_of / should_skip from your original (unchanged) */
static const char *basename_of(const char *path){
    const char *p = strrchr(path, '/');
    if (p) return p + 1;
    p = strrchr(path, '\\');
    if (p) return p + 1;
    return path;
}
bool should_skip(const char *path, bool is_dir){
    const char *name = basename_of(path);
    if (strcmp(name, ".DS_Store") == 0) return true;
    if (strcmp(name, "Thumbs.db") == 0) return true;
    if (strcmp(name, "desktop.ini") == 0) return true;
    if (strcmp(name, "Icon\r") == 0) return true;
    if (strcmp(name, ".Spotlight-V100") == 0) return true;
    if (strcmp(name, ".Trashes") == 0) return true;
    if (strcmp(name, ".fseventsd") == 0) return true;
    if (strcmp(name, ".TemporaryItems") == 0) return true;
    if (strcmp(name, ".git") == 0) return true;
    if (strcmp(name, ".gitignore") == 0) return true;
    if (strcmp(name, ".svn") == 0) return true;
    if (strcmp(name, ".hg") == 0) return true;
    if (strcmp(name, ".bzr") == 0) return true;
    if (strcmp(name, ".fslckout") == 0) return true;

    const char *globs[] = {
        "*~", "*~.*", "#*#", ".#*", "*.swp", "*.swo", "*.swx", ".*.swp",
        ".~lock.*", "*.tmp", "*.temp", "*.part", "*.crdownload", "*.partial",
        "~$*", ".~*", "._*", "ehthumbs.db"
    };
    for (size_t i = 0; i < sizeof(globs)/sizeof(globs[0]); ++i){
        if (fnmatch(globs[i], name, 0) == 0) return true;
    }

    if (name[0] == '~') return true;
    if (strstr(name, ".swp") || strstr(name, ".swo")) return true;

    // 新增：忽略所有以 -shm 或 -wal 结尾的文件
    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, "-shm") == 0) return true;
    if (len > 4 && strcmp(name + len - 4, "-wal") == 0) return true;

    if (is_dir){
        if (strcmp(name, "__MACOSX") == 0) return true;
        if (strcmp(name, "node_modules") == 0) return true;
    }
    return false;
}

int64_t mach_absolute_time_to_us(uint64_t ticks) {
    static mach_timebase_info_data_t timebase = {0,0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t ns = ticks * timebase.numer / timebase.denom;
    return ns / 1000;
}
/* ---------- LevelDB helpers with locking ---------- */

/* get_value_from_leveldb: thread-safe read wrapper
 * returns malloc'd buffer (value), sets *vlen; caller must free().
 * returns NULL if not found or error.
 */
char *get_value_from_leveldb(leveldb_t *db, const char *key, size_t *vlen) {
    if (!db || !key || !vlen) return NULL;

    char *err = NULL;
    char *value = NULL;
    size_t val_len = 0;

    /* --- create and destroy read options properly (was leaking) --- */
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    value = leveldb_get(db, ro, key, strlen(key), &val_len, &err);
    leveldb_readoptions_destroy(ro);

    if (err != NULL) {
        fprintf(stderr, "leveldb_get error: %s\n", err);
        leveldb_free(err);
        return NULL;
    }
    if (value == NULL) return NULL;

    *vlen = val_len;
    return value;
}

int64_t extract_middle(const char *str) {
    char buf[21];
    memcpy(buf, str + 11, 20);
    buf[20] = '\0';
    return strtoll(buf, NULL, 10);
}
uint64_t extract_end(const char *str) {
    char buf[21];
    memcpy(buf, str + 32, 20);
    buf[20] = '\0';
    return strtoull(buf, NULL, 10);
}
uint8_t *get_encoded_payload_by_prefix(const char *prefix, size_t *encoded_len) {
    if (!db || !prefix || !encoded_len) return NULL;
    *encoded_len = 0;
    size_t prefix_len = strlen(prefix);
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    leveldb_iter_seek(it, prefix, prefix_len);
    FileEventBatch payload = FileEventBatch_init_default;
    payload.files_count = 0;
    payload.files = NULL;
    
    size_t capacity = 8;
    payload.files = calloc(capacity, sizeof(pb_bytes_array_t *));
    if (!payload.files) goto fail;
    if(strlen(prefix)>32)leveldb_iter_next(it);
    while (leveldb_iter_valid(it)) {
        size_t key_len = 0;
        const char *key = leveldb_iter_key(it, &key_len);
        if (key_len < prefix_len || strncmp(key, prefix, 11) != 0)
            break;
        size_t val_len = 0;
        const char *val = leveldb_iter_value(it, &val_len);

        pb_bytes_array_t *bytes = malloc(PB_BYTES_ARRAY_T_ALLOCSIZE(val_len));
        if (!bytes) goto fail;
        bytes->size = (pb_size_t)val_len;
        memcpy(bytes->bytes, val, val_len);

        if (payload.files_count >= capacity) {
            capacity *= 2;
            pb_bytes_array_t **tmp = realloc(payload.files, capacity * sizeof(pb_bytes_array_t *));
            if (!tmp) {
                free(bytes);
                goto fail;
            }
            payload.files = tmp;
        }

        payload.files[payload.files_count++] = bytes;
        payload.lastUpdatedTime = extract_middle(key);
        payload.eventId = extract_end(key);
        leveldb_iter_next(it);
    }
    if(payload.files_count==0){
        uint64_t ticks = mach_absolute_time();
        int64_t timeNow = mach_absolute_time_to_us(ticks) + startUpTime;
        payload.lastUpdatedTime = timeNow;
    }
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);

    /* ---------- Nanopb encode ---------- */
    size_t buf_size = 4096 + payload.files_count * 1024; // 预估容量
    uint8_t *buffer = malloc(buf_size);
    if (!buffer) goto fail;

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, buf_size);
    bool status = pb_encode(&stream, FileEventBatch_fields, &payload);
    if (!status) {
        fprintf(stderr, "Encode failed: %s\n", PB_GET_ERROR(&stream));
        free(buffer);
        goto fail;
    }

    *encoded_len = stream.bytes_written;

    uint8_t *encoded = malloc(*encoded_len);
    if (!encoded) {
        free(buffer);
        goto fail;
    }
    memcpy(encoded, buffer, *encoded_len);
    free(buffer);

    /* ---------- cleanup ---------- */
    for (size_t i = 0; i < payload.files_count; i++) {
        free(payload.files[i]);
    }
    free(payload.files);
    return encoded;

fail:
    if (payload.files) {
        for (size_t i = 0; i < payload.files_count; i++) {
            free(payload.files[i]);
        }
        free(payload.files);
    }
    if (it) leveldb_iter_destroy(it);
    if (ro) leveldb_readoptions_destroy(ro);
    *encoded_len = 0;
    return NULL;
}
/* put_value_to_leveldb: thread-safe write wrapper */
int put_value_to_leveldb(const char *key, const char *value, size_t vlen) {
    char *err = NULL;
    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_put(db, woptions, key, strlen(key), value, vlen, &err);
    leveldb_writeoptions_destroy(woptions);
    if (err) {
        fprintf(stderr, "LevelDB put failed: %s\n", err);
        leveldb_free(err);
        return -1;
    }
    return 0;
}

int delete_value_from_leveldb(const char *key) {
    if (!key) return -1;

    char *err = NULL;
    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_delete(db, woptions, key, strlen(key), &err);
    leveldb_writeoptions_destroy(woptions);
    if (err) {
        fprintf(stderr, "LevelDB delete failed: %s\n", err);
        leveldb_free(err);
        return -1;
    }
    return 0;
}

/* ---------- Your original process_path but using the write wrapper ---------- */
static void persist_job_event(Job *job) {
    if (!job || !job->path || !job->root) {
        goto cleanup;
    }

    FileEventMeta file = FileEventMeta_init_default;
    uint8_t buffer[4096];
    bool status;
    size_t message_length;
    uint64_t ticks = mach_absolute_time();
    int64_t time_now = mach_absolute_time_to_us(ticks) + startUpTime;

    file.path = strdup(job->path);
    file.flag = job->flag;

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    status = pb_encode(&stream, FileEventMeta_fields, &file);
    message_length = stream.bytes_written;
    if (!status) {
        printf("Encode failed: %s\n", PB_GET_ERROR(&stream));
        goto cleanup;
    }

    int repoid = 0;
    if (!repo_map_get_repoid(job->root, &repoid)) {
        fprintf(stderr, "Failed to resolve repo for %s\n", job->root);
        goto cleanup;
    }

    uint64_t event_id = job->eventid ? job->eventid : (uint64_t)time_now;
    char key[128];
    snprintf(key, sizeof(key), "1:%08d:%020lld:%020llu", repoid, time_now, event_id);

    if (put_value_to_leveldb(key, (const char *)buffer, message_length) != 0) {
        fprintf(stderr, "Failed to put key %s\n", key);
        goto cleanup;
    }
    printf("✅ Saved file meta: %s\n", key);

cleanup:
    if (file.path) free(file.path);
}

static void process_path(Job *job) {
    persist_job_event(job);

    if (job) {
        if (job->path) free(job->path);
        if (job->root) free(job->root);
        free(job);
    }
}

static void process_dir(Job *job) {
    if (!job) return;

    if (!(job->flag & kFSEventStreamEventFlagItemIsDir)) {
        job->flag |= kFSEventStreamEventFlagItemIsDir;
    }

    persist_job_event(job);

    if (job->path) free(job->path);
    if (job->root) free(job->root);
    free(job);
}

/* dispatch wrappers */
static void dispatch_worker_func(void *vjob) {
    Job *job = (Job*)vjob;
    process_path(job);
}
static void dispatch_worker_func_dir(void *vjob) {
    Job *job = (Job*)vjob;
    process_dir(job);
}


static FSEventsStream *create_stream_context(const char *path, FSEventStreamEventId since_id) {
    if (!path) return NULL;

    FSEventsStream *ctx = calloc(1, sizeof(FSEventsStream));
    if (!ctx) return NULL;

    ctx->work_q = work_q;
    ctx->root = strdup(path);
    if (!ctx->root) {
        free(ctx);
        return NULL;
    }

    ctx->fs_ctx = calloc(1, sizeof(FSEventStreamContext));
    if (!ctx->fs_ctx) {
        free(ctx->root);
        free(ctx);
        return NULL;
    }

    ctx->fs_ctx->info = ctx;

    CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
    if (!arr) {
        free(ctx->fs_ctx);
        ctx->fs_ctx = NULL;
        free(ctx->root);
        free(ctx);
        return NULL;
    }

    CFStringRef s = CFStringCreateWithCString(NULL, path, kCFStringEncodingUTF8);
    if (!s) {
        CFRelease(arr);
        free(ctx->fs_ctx);
        ctx->fs_ctx = NULL;
        free(ctx->root);
        free(ctx);
        return NULL;
    }
    CFArrayAppendValue(arr, s);
    CFRelease(s);

    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagIgnoreSelf |
        kFSEventStreamCreateFlagWatchRoot;

    FSEventStreamEventId since_value = since_id ? since_id : kFSEventStreamEventIdSinceNow;
    ctx->stream = FSEventStreamCreate(NULL, &fsevent_callback, ctx->fs_ctx, arr,
                                      since_value, 0.01, flags);
    CFRelease(arr);
    if (!ctx->stream) {
        free(ctx->fs_ctx);
        ctx->fs_ctx = NULL;
        free(ctx->root);
        free(ctx);
        return NULL;
    }

    FSEventStreamSetDispatchQueue(ctx->stream, work_q);
    if (!FSEventStreamStart(ctx->stream)) {
        FSEventStreamStop(ctx->stream);
        FSEventStreamInvalidate(ctx->stream);
        FSEventStreamRelease(ctx->stream);
        ctx->stream = NULL;
        free(ctx->fs_ctx);
        ctx->fs_ctx = NULL;
        free(ctx->root);
        free(ctx);
        return NULL;
    }

    return ctx;
}

static void restart_timer_handler(void *ctx);
static void restart_timer_cancel(void *ctx);

static RestartWatcher *restart_watcher_lookup(const char *path) {
    RestartWatcher *watcher = NULL;
    HASH_FIND_STR(g_restart_watchers, path, watcher);
    return watcher;
}

static void restart_watcher_cancel_ptr(RestartWatcher *watcher) {
    if (!watcher) return;

    pthread_mutex_lock(&g_restart_lock);
    RestartWatcher *found = restart_watcher_lookup(watcher->path);
    if (found == watcher) {
        HASH_DEL(g_restart_watchers, watcher);
        pthread_mutex_unlock(&g_restart_lock);
        dispatch_source_cancel(watcher->timer);
        return;
    }
    pthread_mutex_unlock(&g_restart_lock);
}

static void cancel_restart_monitor(const char *path) {
    if (!path) return;

    pthread_mutex_lock(&g_restart_lock);
    RestartWatcher *watcher = restart_watcher_lookup(path);
    if (!watcher) {
        pthread_mutex_unlock(&g_restart_lock);
        return;
    }
    HASH_DEL(g_restart_watchers, watcher);
    pthread_mutex_unlock(&g_restart_lock);
    dispatch_source_cancel(watcher->timer);
}

static bool restart_stream_for_path(const char *path, FSEventStreamEventId since_id) {
    if (!path) return false;

    if (access(path, F_OK) != 0) {
        return false;
    }

    int repoid = 0;
    if (!repo_map_get_repoid(path, &repoid)) {
        return false;
    }

    FSEventsStream *ctx = create_stream_context(path, since_id);
    if (!ctx) {
        return false;
    }

    repo_map_set_stream(path, ctx);
    printf("🔁 Restarted watcher for %s\n", path);
    return true;
}

static bool restart_watcher_register(const char *path, FSEventStreamEventId event_id) {
    if (!path || !path[0]) return false;

    pthread_mutex_lock(&g_restart_lock);
    RestartWatcher *existing = restart_watcher_lookup(path);
    if (existing) {
        if (event_id > existing->resume_from) {
            existing->resume_from = event_id;
        }
        pthread_mutex_unlock(&g_restart_lock);
        return true;
    }

    RestartWatcher *watcher = calloc(1, sizeof(RestartWatcher));
    if (!watcher) {
        pthread_mutex_unlock(&g_restart_lock);
        return false;
    }

    strncpy(watcher->path, path, sizeof(watcher->path) - 1);
    watcher->resume_from = event_id;

    dispatch_queue_t queue = dispatch_get_global_queue(DISPATCH_QUEUE_PRIORITY_DEFAULT, 0);
    watcher->timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, queue);
    if (!watcher->timer) {
        free(watcher);
        pthread_mutex_unlock(&g_restart_lock);
        return false;
    }

    dispatch_set_context(watcher->timer, watcher);
    dispatch_source_set_event_handler_f(watcher->timer, restart_timer_handler);
    dispatch_source_set_cancel_handler_f(watcher->timer, restart_timer_cancel);
    dispatch_source_set_timer(watcher->timer,
                              dispatch_time(DISPATCH_TIME_NOW, (int64_t)NSEC_PER_SEC),
                              (uint64_t)(5 * NSEC_PER_SEC),
                              (uint64_t)(NSEC_PER_SEC / 4));

    HASH_ADD_STR(g_restart_watchers, path, watcher);
    pthread_mutex_unlock(&g_restart_lock);

    dispatch_resume(watcher->timer);
    printf("⏳ Waiting for %s to reappear before restarting stream\n", watcher->path);
    return true;
}

static void restart_timer_handler(void *ctx) {
    RestartWatcher *watcher = (RestartWatcher *)ctx;
    if (!watcher) return;

    if (!repo_map_get_repoid(watcher->path, NULL)) {
        restart_watcher_cancel_ptr(watcher);
        return;
    }

    if (!restart_stream_for_path(watcher->path, watcher->resume_from)) {
        return;
    }

    restart_watcher_cancel_ptr(watcher);
}

static void restart_timer_cancel(void *ctx) {
    RestartWatcher *watcher = (RestartWatcher *)ctx;
    if (!watcher) return;

#if !OS_OBJECT_USE_OBJC
    if (watcher->timer) {
        dispatch_release(watcher->timer);
    }
#endif
    free(watcher);
}


typedef struct {
    char *path;
    FSEventStreamEventId resume_from;
} RootRemovalTask;

static void deactivate_root_stream(void *ctx) {
    RootRemovalTask *task = (RootRemovalTask *)ctx;
    if (!task) return;

    if (task->path) {
        repo_map_set_stream(task->path, NULL);

        if (repo_map_get_repoid(task->path, NULL)) {
            if (!restart_stream_for_path(task->path, task->resume_from)) {
                restart_watcher_register(task->path, task->resume_from);
            }
        }

        free(task->path);
    }

    free(task);
}


static void schedule_full_rescan(FSEventsStream *c) {
    // reocde root path to leveldb send back to the client
    // inform client to do a full rescan

}

/* FSEvents callback (uses dispatch to enqueue jobs) */
static void fsevent_callback( ConstFSEventStreamRef streamRef, void *clientCallBackInfo, size_t numEvents, void *eventPaths, const FSEventStreamEventFlags eventFlags[], const FSEventStreamEventId eventIds[]) {
    FSEventsStream *c = (FSEventsStream*)clientCallBackInfo;
    const char *root = c ? c->root : NULL;
    char **paths = eventPaths;
    bool root_needs_restart = false;
    FSEventStreamEventId resume_from = 0;

    for (size_t i = 0; i < numEvents; i++) {
        const char *p = paths[i];
        FSEventStreamEventFlags flags = eventFlags[i];
        FSEventStreamEventId event_id = eventIds[i];

        if (flags & (kFSEventStreamEventFlagItemIsFile | kFSEventStreamEventFlagItemIsSymlink | kFSEventStreamEventFlagItemIsHardlink)) {
            if (should_skip(p, false)) continue;
        } else if (flags & kFSEventStreamEventFlagItemIsDir) {
            if (should_skip(p, true)) continue;
        }

        printf("path: %s %d\n", p, (int)flags);
        print_fsevent_flags(flags);

        if (flags == (kFSEventStreamEventFlagItemXattrMod | kFSEventStreamEventFlagItemIsFile)) {
            continue;
        }

        if (flags & (kFSEventStreamEventFlagKernelDropped | kFSEventStreamEventFlagUserDropped)) {
            fprintf(stderr, "FSEvents reported dropped events (kernel/user). Scheduling full rescan.\n");
            schedule_full_rescan(c);
            root_needs_restart = true;
            if (event_id > resume_from) {
                resume_from = event_id;
            }
            continue;
        }

        if ((flags & kFSEventStreamEventFlagRootChanged) ||
            ((flags & kFSEventStreamEventFlagItemRemoved) && root && strcmp(p, root) == 0)) {
            root_needs_restart = true;
            if (event_id > resume_from) {
                resume_from = event_id;
            }
        }

        bool is_dir_event = (flags & kFSEventStreamEventFlagItemIsDir) || (root && strcmp(p, root) == 0);
        bool should_record =
            (flags & (kFSEventStreamEventFlagItemRemoved |
                      kFSEventStreamEventFlagItemCreated |
                      kFSEventStreamEventFlagItemRenamed |
                      kFSEventStreamEventFlagItemModified))) ||
            (flags & (kFSEventStreamEventFlagRootChanged |
                      kFSEventStreamEventFlagMount |
                      kFSEventStreamEventFlagUnmount));

        if (!should_record) {
            continue;
        }

        Job *job = malloc(sizeof(Job));
        if (!job) {
            continue;
        }

        job->path = strdup(p);
        job->flag = flags;
        job->eventid = event_id;
        job->root = root ? strdup(root) : NULL;

        if (!job->path || !job->root) {
            if (job->path) free(job->path);
            if (job->root) free(job->root);
            free(job);
            continue;
        }

        if (is_dir_event) {
            job->flag |= kFSEventStreamEventFlagItemIsDir;
            printf("Dir changed: %s (flag 0x%x)\n", p, (int)flags);
            dispatch_async_f(work_q, job, dispatch_worker_func_dir);
        } else {
            dispatch_async_f(work_q, job, dispatch_worker_func);
        }
    }

    if (root_needs_restart && root && root[0]) {
        RootRemovalTask *task = calloc(1, sizeof(RootRemovalTask));
        if (task) {
            task->path = strdup(root);
            task->resume_from = resume_from;
            if (!task->path) {
                free(task);
            } else {
                dispatch_async_f(work_q, task, deactivate_root_stream);
            }
        }
    }
}
/* ---------- Simple HTTP server (multithreaded) ---------- */

/* Helper: send all bytes on socket */
static ssize_t send_all(int fd, const void *buf, size_t len) {
    const char *p = buf;
    size_t left = len;
    while (left > 0) {
        ssize_t n = send(fd, p, left, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += n;
        left -= n;
    }
    return len;
}

/* Basic HTTP response helpers */
static void http_respond_200(int client, const char *msg) {
    if (!msg) msg = "";
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 200 OK\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     strlen(msg), msg);
    send_all(client, buf, n);
}
static void http_respond_400(int client) {
    const char *resp = "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\n\r\n";
    send_all(client, resp, strlen(resp));
}
static void http_respond_404(int client) {
    const char *resp = "HTTP/1.1 404 Not Found\r\nContent-Length: 0\r\n\r\n";
    send_all(client, resp, strlen(resp));
}
static void http_respond_500(int client, const char *msg) {
    char buf[512];
    int n = snprintf(buf, sizeof(buf),
                     "HTTP/1.1 500 Internal Server Error\r\n"
                     "Content-Type: text/plain\r\n"
                     "Content-Length: %zu\r\n\r\n%s",
                     strlen(msg), msg);
    send_all(client, buf, n);
}
static void http_respond_405(int client_fd) {
    const char *msg =
        "HTTP/1.1 405 Method Not Allowed\r\n"
        "Content-Type: text/plain\r\n"
        "Content-Length: 23\r\n"
        "Allow: GET, POST\r\n"
        "Connection: close\r\n"
        "\r\n"
        "405 Method Not Allowed\n";
    send_all(client_fd, msg, strlen(msg));
}
/* parse a very simple GET request and extract path+query (no robust parsing) */
static int simple_parse_request(const char *req, char *out_path, size_t out_len) {
    // Expect: GET /some/path?query HTTP/1.1
    const char *p = strstr(req, " ");
    if (!p) return -1;
    p++; // after method
    const char *q = strchr(p, ' ');
    if (!q) return -1;
    size_t len = q - p;
    if (len >= out_len) return -1;
    memcpy(out_path, p, len);
    out_path[len] = '\0';
    return 0;
}
static void url_decode(const char *src, char *dest, size_t dest_size) {
    char a, b;
    size_t i = 0;
    while (*src && i + 1 < dest_size) {
        if ((*src == '%') &&
            ((a = src[1]) && (b = src[2])) &&
            isxdigit(a) && isxdigit(b)) {
            char hex[3] = {a, b, 0};
            dest[i++] = (char) strtol(hex, NULL, 16);
            src += 3;
        } else if (*src == '+') {
            dest[i++] = ' ';
            src++;
        } else {
            dest[i++] = *src++;
        }
    }
    dest[i] = '\0';
}

char *get_param(const char *query, const char *key, char *out, size_t out_size) {
    if (!query || !key || !out || out_size == 0)
        return NULL;

    const char *p = strstr(query, key);
    if (!p) return NULL;

    p += strlen(key);
    if (*p != '=') return NULL;
    p++;

    const char *end = strchr(p, '&');
    size_t len = end ? (size_t)(end - p) : strlen(p);
    if (len >= out_size) len = out_size - 1;

    char encoded[1024];
    if (len >= sizeof(encoded)) len = sizeof(encoded) - 1;
    strncpy(encoded, p, len);
    encoded[len] = '\0';

    url_decode(encoded, out, out_size);
    return out;
}
static void parse_http_method(const char *req, char *method, size_t size) {
    const char *sp = strchr(req, ' ');
    if (!sp) {
        strncpy(method, "UNKNOWN", size);
        return;
    }
    size_t len = sp - req;
    if (len >= size) len = size - 1;
    strncpy(method, req, len);
    method[len] = '\0';
}
/* handle GET /get?repo=<repo>
 * - 示例：返回 raw value bytes as application/octet-stream
 */
static void handle_http_get(int client_fd) {
    char buf[8192];
    ssize_t n = recv(client_fd, buf, sizeof(buf) - 1, 0);
    if (n <= 0) return;
    buf[n] = '\0';

    char path[1024];
    if (simple_parse_request(buf, path, sizeof(path)) != 0) {
        http_respond_400(client_fd);
        return;
    }


    // Route 2: /getprefix?key=<prefix> —— 遍历并返回 Payload
    const char *prefix_scan = "/get?";
    
    // ✅ Route 2: 使用 nanopb 打包 prefix 匹配的所有 FileMeta
    if (strncmp(path, prefix_scan, strlen(prefix_scan)) == 0) {
        const char *query = path + strlen(prefix_scan);
        char workspace[512];
        char lastsync_str[64];
        char eventid_str[64];
        if (!get_param(query, "workspace", workspace, sizeof(workspace)) ||
            !get_param(query, "lastsynctime", lastsync_str, sizeof(lastsync_str)) ||
            !get_param(query, "eventid", eventid_str, sizeof(eventid_str))) {
            http_respond_400(client_fd);
            return;
        }
        int64_t lastsynctime = strtoll(lastsync_str, NULL, 10);
        uint64_t eventid = strtoull(eventid_str, NULL, 10);
        size_t encoded_len = 0;
        int repoid = 0;
        if (!repo_map_get_repoid(workspace, &repoid)) {
            http_respond_404(client_fd);
            return;
        }
        char key[1024];
        if(lastsynctime && eventid){
            snprintf(key, sizeof(key),"1:%08d:%020lld:%020llu", repoid, lastsynctime, eventid);
        }else if(lastsynctime && !eventid){
            snprintf(key, sizeof(key),"1:%08d:%020lld:", repoid, lastsynctime);
        }else{
            snprintf(key, sizeof(key), "1:%08d:", repoid);
        }
        
        uint8_t *encoded_payload = get_encoded_payload_by_prefix( key, &encoded_len);
        if (!encoded_payload) {
            http_respond_404(client_fd);
            return;
        }

        char hdr[256];
        int hlen = snprintf(hdr, sizeof(hdr),
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: application/octet-stream\r\n"
                            "Content-Length: %zu\r\n"
                            "Connection: close\r\n\r\n",
                            encoded_len);
        send_all(client_fd, hdr, hlen);
        send_all(client_fd, encoded_payload, encoded_len);
        free(encoded_payload);
        return;
    }

    // 未匹配到路径
    http_respond_404(client_fd);
}
/* handle post /regist
   handle post /close
*/
static void handle_http_post(int client_fd) {
    char header_buf[8192];
    ssize_t n = recv(client_fd, header_buf, sizeof(header_buf) - 1, 0);
    if (n <= 0) return;
    header_buf[n] = '\0';

    const char *body = strstr(header_buf, "\r\n\r\n");
    if (!body) {
        http_respond_400(client_fd);
        return;
    }
    body += 4;

    const char *cl_hdr = strcasestr(header_buf, "Content-Length:");
    size_t content_len = cl_hdr ? strtoul(cl_hdr + 15, NULL, 10) : 0;
    if (content_len > 16384) {  // 防止过大 body
        http_respond_400(client_fd);
        return;
    }

    size_t header_len = body - header_buf;
    size_t received_body = n - header_len;

    // 分配 body 缓冲区
    char *body_buf = malloc(content_len + 1);
    if (!body_buf) {
        http_respond_500(client_fd, "OOM");
        return;
    }
    memcpy(body_buf, body, received_body);

    while (received_body < content_len) {
        ssize_t m = recv(client_fd, body_buf + received_body, content_len - received_body, 0);
        if (m <= 0) break;
        received_body += m;
    }
    body_buf[received_body] = '\0';

    char path[256];
    if (simple_parse_request(header_buf, path, sizeof(path)) != 0) {
        http_respond_400(client_fd);
        goto cleanup;
    }

    if (strcmp(path, "/regist") == 0) {
        RegisterDirectoryRequest registerdir = RegisterDirectoryRequest_init_default;
        pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t *)body_buf, received_body);

        if (!pb_decode(&stream, RegisterDirectoryRequest_fields, &registerdir)) {
            pb_release(RegisterDirectoryRequest_fields, &registerdir);
            http_respond_400(client_fd);
            goto cleanup;
        }

        if (access(registerdir.path, F_OK) != 0) {
            pb_release(RegisterDirectoryRequest_fields, &registerdir);
            http_respond_400(client_fd);
            goto cleanup;
        }

        cancel_restart_monitor(registerdir.path);

        bool already_registered = repo_map_get_repoid(registerdir.path, NULL);
        FSEventsStream *ctx = NULL;
        if (!already_registered) {
            ctx = create_stream_context(registerdir.path, 0);
            if (!ctx) {
                pb_release(RegisterDirectoryRequest_fields, &registerdir);
                http_respond_500(client_fd, "Stream create failed");
                goto cleanup;
            }
        }

        if (!already_registered) {
            int max_id = 0;
            int exist_id = 0;
            leveldb_readoptions_t *ro = leveldb_readoptions_create();
            leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
            leveldb_iter_seek(it, "3:", 2);
            while (leveldb_iter_valid(it)) {
                size_t klen, vlen;
                const char *key = leveldb_iter_key(it, &klen);
                if (klen < 2 || strncmp(key, "3:", 2) != 0) break;
                const char *val = leveldb_iter_value(it, &vlen);
                int id = atoi(key + 2);
                if (id > max_id) max_id = id;

                char workspace[512];
                memcpy(workspace, val, vlen < sizeof(workspace)-1 ? vlen : sizeof(workspace)-1);
                workspace[vlen] = '\0';
                if (strcmp(registerdir.path, workspace) == 0) exist_id = id;

                leveldb_iter_next(it);
            }
            leveldb_iter_destroy(it);
            leveldb_readoptions_destroy(ro);

            int repoid = exist_id ? exist_id : (max_id + 1);
            repo_map_add(registerdir.path, repoid, ctx);

            char key[20];
            snprintf(key, sizeof(key), "3:%08d", repoid);
            put_value_to_leveldb(key, registerdir.path, strlen(registerdir.path));

            pb_release(RegisterDirectoryRequest_fields, &registerdir);

            uint64_t ticks = mach_absolute_time();
            int64_t timeNow = mach_absolute_time_to_us(ticks) + startUpTime;
            char resp[32];
            snprintf(resp, sizeof(resp), "%lld", timeNow);
            http_respond_200(client_fd, resp);
            goto cleanup;
        }

        pb_release(RegisterDirectoryRequest_fields, &registerdir);
        uint64_t ticks = mach_absolute_time();
        int64_t timeNow = mach_absolute_time_to_us(ticks) + startUpTime;
        char resp[32];
        snprintf(resp, sizeof(resp), "%lld", timeNow);
        http_respond_200(client_fd, resp);
        goto cleanup;
    }
    if (strcmp(path, "/close") == 0) {
        RegisterDirectoryRequest request = RegisterDirectoryRequest_init_default;
        pb_istream_t stream = pb_istream_from_buffer((const pb_byte_t *)body_buf, received_body);
        if (!pb_decode(&stream, RegisterDirectoryRequest_fields, &request)) {
            pb_release(RegisterDirectoryRequest_fields, &request);
            http_respond_400(client_fd);
            goto close_cleanup;
        }

        if (!request.path || request.path[0] == '\0') {
            pb_release(RegisterDirectoryRequest_fields, &request);
            http_respond_400(client_fd);
            goto close_cleanup;
        }

        cancel_restart_monitor(request.path);

        int repoid = -1;
        if (!repo_map_remove(request.path, &repoid)) {
            pb_release(RegisterDirectoryRequest_fields, &request);
            http_respond_404(client_fd);
            goto close_cleanup;
        }

        if (repoid >= 0) {
            char key[20];
            snprintf(key, sizeof(key), "3:%08d", repoid);
            if (delete_value_from_leveldb(key) != 0) {
                pb_release(RegisterDirectoryRequest_fields, &request);
                http_respond_500(client_fd, "Failed to remove workspace");
                goto close_cleanup;
            }
        }

        pb_release(RegisterDirectoryRequest_fields, &request);
        http_respond_200(client_fd, "close ok");

close_cleanup:
        goto cleanup;
    }

    http_respond_404(client_fd);

cleanup:
    free(body_buf);
}


/* thread entry for each connection */
static void *http_connection_thread(void *arg) {
    int client_fd = (int)(intptr_t)arg;
    pthread_detach(pthread_self());

    char peek[1024];
    ssize_t n = recv(client_fd, peek, sizeof(peek) - 1, MSG_PEEK);
    if (n <= 0) {
        close(client_fd);
        return NULL;
    }
    peek[n] = '\0';

    char method[16];
    parse_http_method(peek, method, sizeof(method));

    if (strcasecmp(method, "GET") == 0) {
        handle_http_get(client_fd);
    } else if (strcasecmp(method, "POST") == 0) {
        handle_http_post(client_fd);
    } else {
        http_respond_405(client_fd);
    }

    close(client_fd);
    return NULL;
}


/* HTTP accept loop runs in its own thread */
static void *http_server_thread(void *arg) {
    (void)arg;
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return NULL;
    }
    int on = 1;
    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(HTTP_PORT);

    if (bind(server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(server_fd);
        return NULL;
    }
    if (listen(server_fd, LISTEN_BACKLOG) < 0) {
        perror("listen");
        close(server_fd);
        return NULL;
    }

    printf("HTTP server listening on port %d\n", HTTP_PORT);
    while (1) {
        struct sockaddr_in cli;
        socklen_t clilen = sizeof(cli);
        int client_fd = accept(server_fd, (struct sockaddr *)&cli, &clilen);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        pthread_t th;
        // pass client_fd as pointer-sized integer
        if (pthread_create(&th, NULL, http_connection_thread, (void *)(intptr_t)client_fd) != 0) {
            perror("pthread_create");
            close(client_fd);
        }
    }

    close(server_fd);
    return NULL;
}


int64_t get_boot_time_microseconds(void) {
    struct timeval boottime;
    size_t size = sizeof(boottime);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t default_boot_us = ((int64_t)now.tv_sec - 60) * 1000000LL + now.tv_usec;

    if (sysctl(mib, 2, &boottime, &size, NULL, 0) == 0 && boottime.tv_sec > 0) {
        int64_t boot_us = (int64_t)boottime.tv_sec * 1000000LL + (int64_t)boottime.tv_usec;
        if (boot_us > default_boot_us) {
            return default_boot_us;
        }
        return boot_us;
    }
    return default_boot_us;
}
void handle_signal(int sig) {
    char buf[32];
    uint64_t ticks = mach_absolute_time();
    snprintf(buf, sizeof(buf), "%"PRId64,startUpTime+mach_absolute_time_to_us(ticks));
    put_value_to_leveldb("2",(const char *)buf,strlen(buf));
    leveldb_close(db);
    exit(0);
}

/* ---------- main ---------- */

/* prefix key 1 for normal data */
/* 2 for last start up time  */
/* 3 for workspace */
int main(int argc, char *argv[]) {
    signal(SIGTERM, handle_signal);
    signal(SIGINT, handle_signal);
    size_t size;
    size_t prefix_len = 2;
    char *err = NULL;
    char *value = NULL;
    const char *prefix = "3:";
    
    // open leveldb
    leveldb_options_t *options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    db = leveldb_open(options, "/Users/jexyxiong/.vblevel", &err);
    leveldb_options_destroy(options);
    if (err != NULL) {
        printf("open leveldb failed: %s\n", err);
        leveldb_free(err);
        return 1;
    }
    //Get the startup time if not existed in the leveldb
    
    value = get_value_from_leveldb(db,"2",&size);
    if(value != NULL){
        startUpTime = strtoll(value, NULL, 10);
        free(value);
    }else{
        startUpTime = get_boot_time_microseconds();
    }
    
    work_q = dispatch_queue_create(WORKER_QUEUE_LABEL, DISPATCH_QUEUE_SERIAL);
    repo_map_register_work_queue(work_q);
    // Retrieve the workspace stored in leveldb
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db, ro);
    leveldb_iter_seek(it, prefix, prefix_len);
    
    while (leveldb_iter_valid(it)){
        size_t key_len = 0, val_len = 0;
        const char *key = leveldb_iter_key(it, &key_len);
        if (key_len < prefix_len || strncmp(key, prefix, prefix_len) != 0) {
            break;
        }
        const char *val = leveldb_iter_value(it, &val_len);
        int repoid = atoi(key + prefix_len);
        char workspace[512];
        size_t len = val_len < sizeof(workspace) - 1 ? val_len : sizeof(workspace) - 1;
        memcpy(workspace, val, len);
        workspace[len] = '\0';
        if (access(workspace, F_OK) == 0) {
            FSEventsStream *ctx = malloc(sizeof(FSEventsStream));
            memset(ctx, 0, sizeof(FSEventsStream));
            FSEventStreamContext *fs_ctx = malloc(sizeof(FSEventStreamContext));
            memset(fs_ctx, 0, sizeof(FSEventStreamContext));
            ctx->work_q = work_q;
            ctx->fs_ctx = fs_ctx;
            CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);
            CFStringRef s = CFStringCreateWithCString(NULL, workspace, kCFStringEncodingUTF8);
            CFArrayAppendValue(arr, s);
            CFRelease(s);
            fs_ctx->info = ctx;
            FSEventStreamCreateFlags flags = kFSEventStreamCreateFlagFileEvents | kFSEventStreamCreateFlagNoDefer |
            kFSEventStreamCreateFlagIgnoreSelf | kFSEventStreamCreateFlagWatchRoot;
            ctx->stream = FSEventStreamCreate(NULL, &fsevent_callback, fs_ctx, arr, kFSEventStreamEventIdSinceNow, 0.01, flags);
            ctx->root = strdup(workspace);
            CFRelease(arr);
            FSEventStreamSetDispatchQueue(ctx->stream, work_q);
            if (!FSEventStreamStart(ctx->stream)) {
                fprintf(stderr, "Failed to start FSEvent stream\n");
                // cleanup
                FSEventStreamInvalidate(ctx->stream);
                FSEventStreamRelease(ctx->stream);
                free(ctx->root); 
                free(ctx);
                continue;
            }
            
            repo_map_add(workspace, repoid,ctx);
            // creat stream
            
        }
        leveldb_iter_next(it);
    }
    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    
    // spawn HTTP server thread
    pthread_t http_thread;
    if (pthread_create(&http_thread, NULL, http_server_thread, NULL) != 0) {
        perror("pthread_create http thread");
        // continue without http in that case
    } else {
        pthread_detach(http_thread);
    }

    printf("Watching paths... Ctrl-C to exit\n");

    // run CFRunLoop (blocks)
    CFRunLoopRun();

    // cleanup (not usually reached)
    return 0;
}
