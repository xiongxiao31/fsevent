#include "RepoMap.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <CoreServices/CoreServices.h>

RepoMapEntry *g_repo_map = NULL;
pthread_mutex_t gRepoMapLock = PTHREAD_MUTEX_INITIALIZER;

// 辅助函数：释放 FSEventsStream
static void free_fsevents_stream(FSEventsStream *stream) {
    if (!stream) return;

    if (stream->stream) {
        FSEventStreamStop(stream->stream);
        FSEventStreamInvalidate(stream->stream);
        FSEventStreamRelease(stream->stream);
        stream->stream = NULL;
    }
    if (stream->group) {
        dispatch_group_wait(stream->group, DISPATCH_TIME_FOREVER);
        dispatch_release(stream->group);
        stream->group = NULL;
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
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    if (!entry) {
        // 新建 entry
        entry = malloc(sizeof(RepoMapEntry));
        strncpy(entry->workspace, workspace, sizeof(entry->workspace) - 1);
        entry->workspace[sizeof(entry->workspace) - 1] = '\0';
        entry->stream = NULL;
        HASH_ADD_STR(g_repo_map, workspace, entry);
    } else {
        // 如果已有 stream，先释放旧的
        if (entry->stream) {
            free_fsevents_stream(entry->stream);
            entry->stream = NULL;
        }
    }

    entry->repoid = repoid;
    entry->stream = stream; // 绑定新的 stream

    pthread_mutex_unlock(&gRepoMapLock);
}

// 根据 workspace 查找 entry
RepoMapEntry *repo_map_find(const char *workspace) {
    pthread_mutex_lock(&gRepoMapLock);
    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);
    pthread_mutex_unlock(&gRepoMapLock);
    return entry;
}

// 删除指定 workspace 的 entry
bool repo_map_remove(const char *workspace, int *repoid_out) {
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    bool removed = false;
    if (entry) {
        HASH_DEL(g_repo_map, entry);

        if (repoid_out) {
            *repoid_out = entry->repoid;
        }

        if (entry->stream) {
            free_fsevents_stream(entry->stream);
        }

        free(entry);
        removed = true;
    }

    pthread_mutex_unlock(&gRepoMapLock);
    return removed;
}

// 清空整个 repo map
void repo_map_clear(void) {
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *cur, *tmp;
    HASH_ITER(hh, g_repo_map, cur, tmp) {
        HASH_DEL(g_repo_map, cur);

        if (cur->stream) {
            free_fsevents_stream(cur->stream);
        }

        free(cur);
    }

    pthread_mutex_unlock(&gRepoMapLock);
}

// 专门更新已有 entry 的 stream，安全释放旧的 stream
void repo_map_set_stream(const char *workspace, FSEventsStream *stream) {
    pthread_mutex_lock(&gRepoMapLock);

    RepoMapEntry *entry = NULL;
    HASH_FIND_STR(g_repo_map, workspace, entry);

    if (entry) {
        if (entry->stream) {
            free_fsevents_stream(entry->stream);
        }
        entry->stream = stream;
    }

    pthread_mutex_unlock(&gRepoMapLock);
}
