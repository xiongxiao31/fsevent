#pragma once

#include <stddef.h>
#include <stdint.h>

#include "leveldb/c.h"

char *get_value_from_leveldb(leveldb_t *db, const char *key, size_t *vlen);
int put_value_to_leveldb(leveldb_t *db, const char *key, const char *value, size_t vlen);
int delete_value_from_leveldb(leveldb_t *db, const char *key);
uint8_t *get_encoded_payload_by_prefix(leveldb_t *db, const char *prefix, size_t *encoded_len, int64_t fallback_time);
uint64_t get_last_event_id_for_repo(leveldb_t *db, int repoid);
void remove_repo_entries_from_leveldb(leveldb_t *db, int repoid);
