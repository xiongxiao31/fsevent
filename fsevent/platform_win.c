#include "platform_fsevent.h"

#ifndef __APPLE__

struct PlatformState {
    struct leveldb_t *db;
    int64_t *start_up_time;
};

struct PlatformState *platform_state_create(struct leveldb_t *db, int64_t *start_up_time) {
    (void)db;
    (void)start_up_time;
    return NULL;
}

void platform_state_destroy(struct PlatformState *state) {
    (void)state;
}

bool platform_state_register_workspace(struct PlatformState *state,
                                       const char *normalized_path,
                                       int repoid,
                                       uint64_t since_event_id) {
    (void)state;
    (void)normalized_path;
    (void)repoid;
    (void)since_event_id;
    return false;
}

void platform_state_run_loop(struct PlatformState *state) {
    (void)state;
}

void platform_state_handle_shutdown(struct PlatformState *state) {
    (void)state;
}

int64_t platform_state_current_time(const struct PlatformState *state) {
    (void)state;
    return 0;
}

#endif
