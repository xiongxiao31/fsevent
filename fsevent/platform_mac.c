#include "platform_fsevent.h"

#ifdef __APPLE__

#include "FSEventsStream.h"
#include "RepoMap.h"
#include "leveldb_helpers.h"
#include "pb.h"
#include "vbproto_pb.h"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#include <fnmatch.h>
#include <mach/mach_time.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WORKER_QUEUE_LABEL "com.example.fsevent.worker"

typedef struct {
    char *path;
    char *root;
    int64_t flag;
    uint64_t eventid;
} Job;

struct PlatformState {
    leveldb_t *db;
    int64_t *start_up_time;
    dispatch_queue_t work_q;
};

static struct PlatformState *g_state = NULL;

static int64_t mach_absolute_time_to_ns(uint64_t ticks) {
    static mach_timebase_info_data_t timebase = {0, 0};
    if (timebase.denom == 0) {
        mach_timebase_info(&timebase);
    }
    uint64_t ns = ticks * timebase.numer / timebase.denom;
    return (int64_t)ns;
}

static void print_fsevent_flags(FSEventStreamEventFlags flags) {
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

static const char *basename_of(const char *path) {
    const char *p = strrchr(path, '/');
    if (p) return p + 1;
    p = strrchr(path, '\\');
    if (p) return p + 1;
    return path;
}

static bool should_skip(const char *path, bool is_dir) {
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
    if (strcmp(name, ".fslckout-journal") == 0) return true;
    const char *globs[] = {
        "*~", "*~.*", "#*#", ".#*", "*.swp", "*.swo", "*.swx", ".*.swp",
        ".~lock.*", "*.tmp", "*.temp", "*.part", "*.crdownload", "*.partial",
        "~$*", ".~*", "._*", "ehthumbs.db"
    };
    for (size_t i = 0; i < sizeof(globs) / sizeof(globs[0]); ++i) {
        if (fnmatch(globs[i], name, 0) == 0) return true;
    }

    if (name[0] == '~') return true;
    if (strstr(name, ".swp") || strstr(name, ".swo")) return true;

    size_t len = strlen(name);
    if (len > 4 && strcmp(name + len - 4, "-shm") == 0) return true;
    if (len > 4 && strcmp(name + len - 4, "-wal") == 0) return true;

    if (is_dir) {
        if (strcmp(name, "__MACOSX") == 0) return true;
        if (strcmp(name, "node_modules") == 0) return true;
    }
    return false;
}

static void free_job(Job *job) {
    if (!job) return;
    free(job->path);
    free(job->root);
    free(job);
}

static void persist_job_event(Job *job) {
    if (!job || !g_state || !job->path || !job->root) {
        free_job(job);
        return;
    }

    FileEventMeta file = FileEventMeta_init_default;
    uint8_t buffer[4096];
    bool status;
    size_t message_length;
    uint64_t ticks = mach_absolute_time();
    int64_t time_now = mach_absolute_time_to_ns(ticks) + *g_state->start_up_time;

    file.path = strdup(job->path);
    file.flag = job->flag;

    pb_ostream_t stream = pb_ostream_from_buffer(buffer, sizeof(buffer));
    status = pb_encode(&stream, FileEventMeta_fields, &file);
    message_length = stream.bytes_written;
    if (!status) {
        printf("Encode failed: %s\n", PB_GET_ERROR(&stream));
        free(file.path);
        free_job(job);
        return;
    }

    int repoid = 0;
    if (!repo_map_get_repoid(job->root, &repoid)) {
        fprintf(stderr, "Failed to resolve repo for %s\n", job->root);
        free(file.path);
        free_job(job);
        return;
    }

    uint64_t event_id = job->eventid ? job->eventid : (uint64_t)time_now;
    char key[128];
    snprintf(key, sizeof(key), "1:%08d:%020lld:%020llu", repoid, time_now, event_id);

    if (put_value_to_leveldb(g_state->db, key, (const char *)buffer, message_length) != 0) {
        fprintf(stderr, "Failed to put key %s\n", key);
    } else {
        printf("✅ Saved file name: %s\n key:%s\n", file.path, key);
    }

    free(file.path);
    free_job(job);
}

static void persist_dir_event(Job *job) {
    if (!job) return;
    if (!(job->flag & kFSEventStreamEventFlagItemIsDir)) {
        job->flag |= kFSEventStreamEventFlagItemIsDir;
    }
    persist_job_event(job);
}

static void dispatch_worker_func(void *vjob) {
    persist_job_event((Job *)vjob);
}

static void dispatch_worker_func_dir(void *vjob) {
    persist_dir_event((Job *)vjob);
}

static void schedule_full_rescan(FSEventsStream *c) {
    (void)c;
    // TODO: trigger a full rescan notification.
}

static void fsevent_callback(ConstFSEventStreamRef streamRef,
                             void *clientCallBackInfo,
                             size_t numEvents,
                             void *eventPaths,
                             const FSEventStreamEventFlags eventFlags[],
                             const FSEventStreamEventId eventIds[]) {
    (void)streamRef;
    FSEventsStream *c = (FSEventsStream *)clientCallBackInfo;
    if (!c || !g_state) return;

    char *root = strdup(c->root);
    char **paths = eventPaths;
    bool root_changed = false;
    for (size_t i = 0; i < numEvents; i++) {
        const char *p = paths[i];
        FSEventStreamEventFlags flags = eventFlags[i];

        if (flags & kFSEventStreamEventFlagRootChanged) {
            root_changed = true;
            fprintf(stderr, "Root changed detected for %s, cleaning up.\n", root);
            continue;
        }

        if (flags & (kFSEventStreamEventFlagItemIsFile |
                     kFSEventStreamEventFlagItemIsSymlink |
                     kFSEventStreamEventFlagItemIsHardlink)) {
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
            continue;
        }

        if (flags & (kFSEventStreamEventFlagItemRemoved |
                     kFSEventStreamEventFlagItemCreated |
                     kFSEventStreamEventFlagItemRenamed |
                     kFSEventStreamEventFlagItemModified)) {
            Job *job = malloc(sizeof(Job));
            if (!job) continue;
            job->path = strdup(p);
            job->flag = flags;
            job->eventid = eventIds[i];
            job->root = strdup(root);
            if (!job->path || !job->root) {
                free_job(job);
                continue;
            }
            if (flags & (kFSEventStreamEventFlagItemIsFile |
                         kFSEventStreamEventFlagItemIsSymlink |
                         kFSEventStreamEventFlagItemIsHardlink)) {
                dispatch_async_f(g_state->work_q, job, dispatch_worker_func);
            } else if (flags & kFSEventStreamEventFlagItemIsDir) {
                dispatch_async_f(g_state->work_q, job, dispatch_worker_func_dir);
            } else {
                free_job(job);
            }
        }
    }

    if (root_changed) {
        int repoid = -1;
        if (repo_map_remove(root, &repoid)) {
            remove_repo_entries_from_leveldb(g_state->db, repoid);
        } else {
            fprintf(stderr, "Failed to remove repo for %s after root change\n", root);
        }
    }
    free(root);
}

struct PlatformState *platform_state_create(leveldb_t *db, int64_t *start_up_time) {
    struct PlatformState *state = calloc(1, sizeof(struct PlatformState));
    if (!state) {
        return NULL;
    }

    state->db = db;
    state->start_up_time = start_up_time;
    state->work_q = dispatch_queue_create(WORKER_QUEUE_LABEL, DISPATCH_QUEUE_SERIAL);
    if (!state->work_q) {
        free(state);
        return NULL;
    }

    repo_map_register_work_queue(state->work_q);
    g_state = state;
    return state;
}

void platform_state_destroy(struct PlatformState *state) {
    if (!state) return;
    if (g_state == state) {
        g_state = NULL;
    }
#if !defined(OS_OBJECT_USE_OBJC) || !OS_OBJECT_USE_OBJC
    if (state->work_q) {
        dispatch_release(state->work_q);
    }
#endif
    free(state);
}

bool platform_state_register_workspace(struct PlatformState *state,
                                       const char *normalized_path,
                                       int repoid,
                                       uint64_t since_event_id) {
    if (!state || !normalized_path) {
        return false;
    }

    FSEventsStream *ctx = calloc(1, sizeof(FSEventsStream));
    FSEventStreamContext *fs_ctx = calloc(1, sizeof(FSEventStreamContext));
    CFMutableArrayRef arr = CFArrayCreateMutable(NULL, 0, &kCFTypeArrayCallBacks);

    if (!ctx || !fs_ctx || !arr) {
        if (arr) CFRelease(arr);
        free(ctx);
        free(fs_ctx);
        return false;
    }

    CFStringRef s = CFStringCreateWithCString(NULL, normalized_path, kCFStringEncodingUTF8);
    CFArrayAppendValue(arr, s);
    CFRelease(s);

    fs_ctx->info = ctx;
    ctx->fs_ctx = fs_ctx;
    ctx->work_q = state->work_q;
    ctx->root = strdup(normalized_path);
    if (!ctx->root) {
        CFRelease(arr);
        free(ctx);
        free(fs_ctx);
        return false;
    }

    FSEventStreamCreateFlags flags =
        kFSEventStreamCreateFlagFileEvents |
        kFSEventStreamCreateFlagNoDefer |
        kFSEventStreamCreateFlagIgnoreSelf |
        kFSEventStreamCreateFlagWatchRoot;

    FSEventStreamEventId since_when = since_event_id > 0 ? since_event_id : kFSEventStreamEventIdSinceNow;
    ctx->stream = FSEventStreamCreate(NULL, &fsevent_callback, fs_ctx, arr, since_when, 0.0000001, flags);
    CFRelease(arr);

    if (!ctx->stream) {
        free(ctx->root);
        free(fs_ctx);
        free(ctx);
        return false;
    }

    FSEventStreamSetDispatchQueue(ctx->stream, state->work_q);
    if (!FSEventStreamStart(ctx->stream)) {
        FSEventStreamInvalidate(ctx->stream);
        FSEventStreamRelease(ctx->stream);
        free(ctx->root);
        free(fs_ctx);
        free(ctx);
        return false;
    }

    repo_map_add(normalized_path, repoid, ctx);
    return true;
}

void platform_state_run_loop(struct PlatformState *state) {
    (void)state;
    CFRunLoopRun();
}

void platform_state_handle_shutdown(struct PlatformState *state) {
    if (!state) return;
    uint64_t ticks = mach_absolute_time();
    int64_t now = mach_absolute_time_to_ns(ticks) + *state->start_up_time;
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", now);
    put_value_to_leveldb(state->db, "2", buf, strlen(buf));
}

int64_t platform_state_current_time(const struct PlatformState *state) {
    if (!state) return 0;
    uint64_t ticks = mach_absolute_time();
    return mach_absolute_time_to_ns(ticks) + *state->start_up_time;
}

#endif // __APPLE__
