#pragma once

#ifdef __APPLE__
#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>
#else
#include <stdint.h>
typedef void *FSEventStreamRef;
typedef uint64_t FSEventStreamEventId;
typedef void *dispatch_queue_t;
typedef void *dispatch_group_t;
typedef struct FSEventStreamContext FSEventStreamContext;
#endif

typedef struct FSEventsStream {
    dispatch_queue_t work_q;
    FSEventStreamRef stream;
    char *root;
    FSEventStreamEventId lastGood;
    FSEventStreamEventId since;
    dispatch_group_t group;
    FSEventStreamContext *fs_ctx;
} FSEventsStream;
