#pragma once
#include "uthash.h"
#include <dispatch/dispatch.h>
#include <CoreServices/CoreServices.h>
typedef struct {
    dispatch_queue_t work_q;
    FSEventStreamRef stream;
    char *root;
    FSEventStreamEventId lastGood;
    FSEventStreamEventId since;
    dispatch_group_t group;
    FSEventStreamContext *fs_ctx;
} FSEventsStream;
typedef struct {
    char workspace[512];
    int repoid;
    FSEventsStream *stream;
    UT_hash_handle hh;
} RepoMapEntry;

extern RepoMapEntry *g_repo_map;

void repo_map_add(const char *workspace, int repoid, FSEventsStream *stream);

// 查找 entry
RepoMapEntry *repo_map_find(const char *workspace);

// 删除 entry
void repo_map_remove(const char *workspace);

// 清空整个 hash 表
void repo_map_clear(void);

// 更新已有 entry 的 FSEventsStream，安全释放旧 stream
void repo_map_set_stream(const char *workspace, FSEventsStream *stream);
