#pragma once

#include <stdbool.h>
#include <stdint.h>

struct PlatformState;
struct leveldb_t;

struct PlatformState *platform_state_create(struct leveldb_t *db, int64_t *start_up_time);
void platform_state_destroy(struct PlatformState *state);

bool platform_state_register_workspace(struct PlatformState *state,
                                       const char *normalized_path,
                                       int repoid,
                                       uint64_t since_event_id);

void platform_state_run_loop(struct PlatformState *state);
void platform_state_handle_shutdown(struct PlatformState *state);
int64_t platform_state_current_time(const struct PlatformState *state);

