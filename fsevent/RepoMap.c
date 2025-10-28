#include "RepoMap.h"
#include "FSEventsStream.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

RepoMapEntry *g_repo_map = NULL;
pthread_mutex_t gRepoMapLock = PTHREAD_MUTEX_INITIALIZER;

static int repo_map_queue_key;

static void sync_noop(void *ctx) {
    (void)ctx;
}

static bool is_on_work_queue(void) {
    return dispatch_get_specific(&repo_map_queue_key) != NULL;
}

void repo_map_register_work_queue(dispatch_queue_t queue) {
    dispatch_queue_set_specific(queue, &repo_map_queue_key, &repo_map_queue_key, NULL);
}

// 辅助函数：释放 FSEventsStream
static void free_fsevents_stream(FSEventsStream *stream) {
    if (!stream) return;

    if (stream->stream) {
        FSEventStreamStop(stream->stream);
        FSEventStreamInvalidate(stream->stream);
        FSEventStreamRelease(stream->stream);
        stream->stream = NULL;
    }
    if (stream->work_q && !is_on_work_queue()) {
        dispatch_sync_f(stream->work_q, NULL, sync_noop);
    }
    if (stream->group) {
        dispatch_group_wait(stream->group, DISPATCH_TIME_FOREVER);
        dispatch_release(stream->group);
        stream->group = NULL;
    }
    if (stream->fs_ctx) {
        stream->fs_ctx->info = NULL;
    }
    if (stream->root) {
        free(stream->root);
        stream->root = NULL;
    }
    free(stream->fs_ctx);
    free(stream);
}

// 添加或更新 RepoMapEntry，同时可绑定 FSEventsStream
void repo_map_add(const char *workspace, int repoid, FSEventsStream *stream) {
    FSEventsStream *old_stream = NULL;
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    if (!entry) {
        // 新建 entry
        entry = malloc(sizeof(RepoMapEntry));
        if (!entry) {
            pthread_mutex_unlock(&gRepoMapLock);
            free_fsevents_stream(stream);
            return;
        }
        strncpy(entry->workspace, workspace, sizeof(entry->workspace) - 1);
        entry->workspace[sizeof(entry->workspace) - 1] = '\0';
        entry->stream = NULL;
        HASH_ADD_STR(g_repo_map, workspace, entry);
    } else {
        // 如果已有 stream，先释放旧的
        if (entry->stream) {
            old_stream = entry->stream;
            entry->stream = NULL;
        }
    }

    entry->repoid = repoid;
    entry->stream = stream; // 绑定新的 stream

    pthread_mutex_unlock(&gRepoMapLock);
    if (old_stream) {
        free_fsevents_stream(old_stream);
    }
}

// 根据 workspace 查 repoid
bool repo_map_get_repoid(const char *workspace, int *repoid_out) {
    bool found = false;
    pthread_mutex_lock(&gRepoMapLock);
    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);
    if (entry) {
        if (repoid_out) {
            *repoid_out = entry->repoid;
        }
        found = true;
    }
    pthread_mutex_unlock(&gRepoMapLock);
    return found;
}

// 删除指定 workspace 的 entry
bool repo_map_remove(const char *workspace, int *repoid_out) {
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    bool removed = false;
    FSEventsStream *stream = NULL;
    if (entry) {
        HASH_DEL(g_repo_map, entry);

        if (repoid_out) {
            *repoid_out = entry->repoid;
        }

        stream = entry->stream;
        entry->stream = NULL;
        free(entry);
        removed = true;
    }

    pthread_mutex_unlock(&gRepoMapLock);
    if (stream) {
        free_fsevents_stream(stream);
    }
    return removed;
}

// 清空整个 repo map
void repo_map_clear(void) {
    while (1) {
        char workspace[sizeof(((RepoMapEntry *)0)->workspace)];

        pthread_mutex_lock(&gRepoMapLock);
        if (!g_repo_map) {
            pthread_mutex_unlock(&gRepoMapLock);
            break;
        }
        RepoMapEntry *entry = g_repo_map;
        strncpy(workspace, entry->workspace, sizeof(workspace) - 1);
        workspace[sizeof(workspace) - 1] = '\0';
        pthread_mutex_unlock(&gRepoMapLock);

        repo_map_remove(workspace, NULL);
    }
}

// 专门更新已有 entry 的 stream，安全释放旧的 stream
void repo_map_set_stream(const char *workspace, FSEventsStream *stream) {
    FSEventsStream *old_stream = NULL;
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    if (entry) {
        if (entry->stream) {
            old_stream = entry->stream;
        }
        entry->stream = stream;
    } else {
        pthread_mutex_unlock(&gRepoMapLock);
        free_fsevents_stream(stream);
        return;
    }

    pthread_mutex_unlock(&gRepoMapLock);
    if (old_stream) {
        free_fsevents_stream(old_stream);
    }
}
