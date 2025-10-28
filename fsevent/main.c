// main.c
// Compile (example):
// clang main.c -framework CoreServices -framework CoreFoundation -lpthread -lleveldb -o fsevent_vcs
//
// 注意：根据你的系统 link flags 和 leveldb 安装方式可能需调整 -lleveldb 路径/选项

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <errno.h>
#include <unistd.h>
#include <stdint.h>
#include <inttypes.h>
#include <time.h>
#include <stdbool.h>
#include <limits.h>
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <arpa/inet.h>
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
#include "leveldb_helpers.h"
#include "platform_fsevent.h"
#define HTTP_PORT 8079
#define LISTEN_BACKLOG 32


int64_t startUpTime;
leveldb_t *db;
static struct PlatformState *platform_state;

static bool ensure_directory_exists(const char *path) {
    if (!path) return false;

    struct stat st;
    if (stat(path, &st) == 0) {
        return S_ISDIR(st.st_mode);
    }

    if (errno != ENOENT) {
        return false;
    }

    if (mkdir(path, 0700) == 0) {
        return true;
    }

    return errno == EEXIST;
}

static bool canonicalize_path(const char *input, char *output, size_t size) {
    if (!input || !output || size == 0) return false;
    char *resolved = realpath(input, output);
    if (resolved) return true;
    return false;
}

static bool is_path_within_home(const char *path) {
    const char *home = getenv("HOME");
    if (!home || !path) return false;

    char resolved_home[PATH_MAX];
    char resolved_path[PATH_MAX];

    if (!canonicalize_path(home, resolved_home, sizeof(resolved_home))) {
        return false;
    }
    if (!canonicalize_path(path, resolved_path, sizeof(resolved_path))) {
        return false;
    }

    size_t home_len = strlen(resolved_home);
    if (home_len == 0) return false;

    if (strncmp(resolved_path, resolved_home, home_len) != 0) {
        return false;
    }

    return resolved_path[home_len] == '\0' || resolved_path[home_len] == '/';
}

static bool build_leveldb_storage_path(char *out, size_t out_size) {
    if (!out || out_size == 0) return false;

    const char *home = getenv("HOME");
    if (!home || home[0] == '\0') {
        return false;
    }

    char intermediate[PATH_MAX];
    int written = snprintf(intermediate, sizeof(intermediate), "%s/Library", home);
    if (written < 0 || (size_t)written >= sizeof(intermediate) || !ensure_directory_exists(intermediate)) {
        return false;
    }

    written = snprintf(intermediate, sizeof(intermediate), "%s/Library/Application Support", home);
    if (written < 0 || (size_t)written >= sizeof(intermediate) || !ensure_directory_exists(intermediate)) {
        return false;
    }

    written = snprintf(intermediate, sizeof(intermediate), "%s/Library/Application Support/FSEventWatcher", home);
    if (written < 0 || (size_t)written >= sizeof(intermediate) || !ensure_directory_exists(intermediate)) {
        return false;
    }

    written = snprintf(out, out_size, "%s/Library/Application Support/FSEventWatcher/LevelDB", home);
    if (written < 0 || (size_t)written >= out_size) {
        return false;
    }

    return ensure_directory_exists(out);
}
/* ---------- LevelDB helpers with locking ---------- */

/* get_value_from_leveldb: thread-safe read wrapper
 * returns LevelDB-owned buffer (value), sets *vlen; caller must release via leveldb_free().
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

uint64_t get_last_event_id_for_repo(leveldb_t *db_handle, int repoid) {
    if (!db_handle || repoid < 0) return 0;

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "1:%08d:", repoid);
    size_t prefix_len = strlen(prefix);

    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db_handle, ro);
    leveldb_iter_seek(it, prefix, prefix_len);

    uint64_t last_event_id = 0;
    while (leveldb_iter_valid(it)) {
        size_t key_len = 0;
        const char *key = leveldb_iter_key(it, &key_len);
        if (key_len < prefix_len || strncmp(key, prefix, prefix_len) != 0) {
            break;
        }

        last_event_id = extract_end(key);
        leveldb_iter_next(it);
    }

    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);
    return last_event_id;
}
uint8_t *get_encoded_payload_by_prefix(leveldb_t *db_handle, const char *prefix, size_t *encoded_len, int64_t fallback_time) {
    if (!db_handle || !prefix || !encoded_len) return NULL;
    *encoded_len = 0;
    size_t prefix_len = strlen(prefix);
    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db_handle, ro);
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
        payload.lastUpdatedTime = fallback_time;
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
int put_value_to_leveldb(leveldb_t *db_handle, const char *key, const char *value, size_t vlen) {
    if (!db_handle || !key || !value) {
        return -1;
    }
    char *err = NULL;
    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_put(db_handle, woptions, key, strlen(key), value, vlen, &err);
    leveldb_writeoptions_destroy(woptions);
    if (err) {
        fprintf(stderr, "LevelDB put failed: %s\n", err);
        leveldb_free(err);
        return -1;
    }
    return 0;
}

int delete_value_from_leveldb(leveldb_t *db_handle, const char *key) {
    if (!db_handle || !key) return -1;

    char *err = NULL;
    leveldb_writeoptions_t *woptions = leveldb_writeoptions_create();
    leveldb_delete(db_handle, woptions, key, strlen(key), &err);
    leveldb_writeoptions_destroy(woptions);
    if (err) {
        fprintf(stderr, "LevelDB delete failed: %s\n", err);
        leveldb_free(err);
        return -1;
    }
    return 0;
}

void remove_repo_entries_from_leveldb(leveldb_t *db_handle, int repoid) {
    if (!db_handle || repoid < 0) return;

    char prefix[32];
    snprintf(prefix, sizeof(prefix), "1:%08d:", repoid);
    size_t prefix_len = strlen(prefix);

    leveldb_readoptions_t *ro = leveldb_readoptions_create();
    leveldb_iterator_t *it = leveldb_create_iterator(db_handle, ro);
    leveldb_iter_seek(it, prefix, prefix_len);

    char **keys = NULL;
    size_t key_count = 0;
    size_t key_cap = 0;

    while (leveldb_iter_valid(it)) {
        size_t key_len = 0;
        const char *key = leveldb_iter_key(it, &key_len);
        if (key_len < prefix_len || strncmp(key, prefix, prefix_len) != 0) {
            break;
        }

        char *copy = malloc(key_len + 1);
        if (!copy) {
            fprintf(stderr, "OOM while collecting keys for repo %d\n", repoid);
            break;
        }

        memcpy(copy, key, key_len);
        copy[key_len] = '\0';

        if (key_count == key_cap) {
            size_t new_cap = key_cap ? key_cap * 2 : 8;
            char **new_keys = realloc(keys, new_cap * sizeof(char *));
            if (!new_keys) {
                fprintf(stderr, "OOM expanding key list for repo %d\n", repoid);
                free(copy);
                break;
            }
            keys = new_keys;
            key_cap = new_cap;
        }

        keys[key_count++] = copy;
        leveldb_iter_next(it);
    }

    leveldb_iter_destroy(it);
    leveldb_readoptions_destroy(ro);

    for (size_t i = 0; i < key_count; ++i) {
        if (delete_value_from_leveldb(db_handle, keys[i]) != 0) {
            fprintf(stderr, "Failed to delete key %s for repo %d\n", keys[i], repoid);
        }
        free(keys[i]);
    }
    free(keys);

    char repo_key[20];
    snprintf(repo_key, sizeof(repo_key), "3:%08d", repoid);
    if (delete_value_from_leveldb(db_handle, repo_key) != 0) {
        fprintf(stderr, "Failed to delete repo key %s\n", repo_key);
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
static void http_respond_403(int client) {
    const char *resp = "HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n";
    send_all(client, resp, strlen(resp));
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

//        if (!is_path_within_home(workspace)) {
//            http_respond_403(client_fd);
//            return;
//        }

        char normalized_workspace[PATH_MAX];
        if (!canonicalize_path(workspace, normalized_workspace, sizeof(normalized_workspace))) {
            http_respond_400(client_fd);
            return;
        }
        int64_t lastsynctime = strtoll(lastsync_str, NULL, 10);
        uint64_t eventid = strtoull(eventid_str, NULL, 10);
        size_t encoded_len = 0;
        int repoid = 0;
        if (!repo_map_get_repoid(normalized_workspace, &repoid)) {
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
        
        int64_t fallback_time = platform_state_current_time(platform_state);
        uint8_t *encoded_payload = get_encoded_payload_by_prefix(db, key, &encoded_len, fallback_time);
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

        struct stat st;
        if (stat(registerdir.path, &st) != 0 || !S_ISDIR(st.st_mode)) {
            pb_release(RegisterDirectoryRequest_fields, &registerdir);
            http_respond_400(client_fd);
            goto cleanup;
        }

//        if (!is_path_within_home(registerdir.path)) {
//            pb_release(RegisterDirectoryRequest_fields, &registerdir);
//            http_respond_403(client_fd);
//            goto cleanup;
//        }

        char normalized_path[PATH_MAX];
        if (!canonicalize_path(registerdir.path, normalized_path, sizeof(normalized_path))) {
            pb_release(RegisterDirectoryRequest_fields, &registerdir);
            http_respond_400(client_fd);
            goto cleanup;
        }

        bool already_registered = repo_map_get_repoid(normalized_path, NULL);
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
                if (strcmp(normalized_path, workspace) == 0) exist_id = id;

                leveldb_iter_next(it);
            }
            leveldb_iter_destroy(it);
            leveldb_readoptions_destroy(ro);

            int repoid = exist_id ? exist_id : (max_id + 1);
            if (!platform_state_register_workspace(platform_state, normalized_path, repoid, 0)) {
                http_respond_500(client_fd, "Stream create failed");
                pb_release(RegisterDirectoryRequest_fields, &registerdir);
                goto cleanup;
            }

            char key[20];
            snprintf(key, sizeof(key), "3:%08d", repoid);
            if (put_value_to_leveldb(db, key, normalized_path, strlen(normalized_path)) != 0) {
                repo_map_remove(normalized_path, NULL);
                http_respond_500(client_fd, "Failed to persist workspace");
                pb_release(RegisterDirectoryRequest_fields, &registerdir);
                goto cleanup;
            }

            pb_release(RegisterDirectoryRequest_fields, &registerdir);

            int64_t timeNow = platform_state_current_time(platform_state);
            char resp[32];
            snprintf(resp, sizeof(resp), "%lld", timeNow);
            http_respond_200(client_fd, resp);
            goto cleanup;
        }

        pb_release(RegisterDirectoryRequest_fields, &registerdir);
        int64_t timeNow = platform_state_current_time(platform_state);
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

//        if (!is_path_within_home(request.path)) {
//            pb_release(RegisterDirectoryRequest_fields, &request);
//            http_respond_403(client_fd);
//            goto close_cleanup;
//        }

        char normalized_path[PATH_MAX];
        if (!canonicalize_path(request.path, normalized_path, sizeof(normalized_path))) {
            pb_release(RegisterDirectoryRequest_fields, &request);
            http_respond_400(client_fd);
            goto close_cleanup;
        }

        int repoid = -1;
        if (!repo_map_remove(normalized_path, &repoid)) {
            pb_release(RegisterDirectoryRequest_fields, &request);
            http_respond_404(client_fd);
            goto close_cleanup;
        }

        if (repoid >= 0) {
            char key[20];
            snprintf(key, sizeof(key), "3:%08d", repoid);
            if (delete_value_from_leveldb(db, key) != 0) {
                pb_release(RegisterDirectoryRequest_fields, &request);
                http_respond_500(client_fd, "Failed to remove workspace");
                goto close_cleanup;
            }
            remove_repo_entries_from_leveldb(db, repoid);
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
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
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


int64_t get_boot_time_nanoseconds(void) {
    struct timeval boottime;
    size_t size = sizeof(boottime);
    int mib[2] = { CTL_KERN, KERN_BOOTTIME };
    struct timeval now;
    gettimeofday(&now, NULL);
    int64_t default_boot_ns =
        ((int64_t)now.tv_sec - 60) * 1000000000LL + (int64_t)now.tv_usec * 1000LL;

    if (sysctl(mib, 2, &boottime, &size, NULL, 0) == 0 && boottime.tv_sec > 0) {
        int64_t boot_ns =
            (int64_t)boottime.tv_sec * 1000000000LL + (int64_t)boottime.tv_usec * 1000LL;
        if (boot_ns > default_boot_ns) {
            return default_boot_ns;
        }
        return boot_ns;
    }
    return default_boot_ns;
}
void handle_signal(int sig) {
    (void)sig;
    platform_state_handle_shutdown(platform_state);
    platform_state_destroy(platform_state);
    platform_state = NULL;
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
    
    char leveldb_path[PATH_MAX];
    if (!build_leveldb_storage_path(leveldb_path, sizeof(leveldb_path))) {
        fprintf(stderr, "Failed to prepare LevelDB container under Application Support\n");
        return 1;
    }

    // open leveldb
    leveldb_options_t *options = leveldb_options_create();
    leveldb_options_set_create_if_missing(options, 1);
    db = leveldb_open(options, leveldb_path, &err);
    leveldb_options_destroy(options);
    if (err != NULL) {
        printf("open leveldb failed: %s\n", err);
        leveldb_free(err);
        return 1;
    }
    //Get the startup time if not existed in the leveldb
    
    value = get_value_from_leveldb(db,"2",&size);
    if (value != NULL) {
        char buf[32];
        size_t copy_len = size < sizeof(buf) - 1 ? size : sizeof(buf) - 1;
        memcpy(buf, value, copy_len);
        buf[copy_len] = '\0';
        startUpTime = strtoll(buf, NULL, 10);
        leveldb_free(value);
        if (startUpTime < 100000000000000000LL) {
            startUpTime *= 1000;
        }
    } else {
        startUpTime = get_boot_time_nanoseconds();
    }
    
    platform_state = platform_state_create(db, &startUpTime);
    if (!platform_state) {
        fprintf(stderr, "Failed to initialize platform state\n");
        return 1;
    }
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
        if (access(workspace, F_OK) == 0 && is_path_within_home(workspace)) {
            struct stat st;
            if (stat(workspace, &st) != 0 || !S_ISDIR(st.st_mode)) {
                leveldb_iter_next(it);
                continue;
            }
            char normalized_workspace[PATH_MAX];
            if (!canonicalize_path(workspace, normalized_workspace, sizeof(normalized_workspace))) {
                leveldb_iter_next(it);
                continue;
            }
            uint64_t since_when = get_last_event_id_for_repo(db, repoid);
            if (!platform_state_register_workspace(platform_state, normalized_workspace, repoid, since_when)) {
                fprintf(stderr, "Failed to restore watcher for %s\n", normalized_workspace);
                leveldb_iter_next(it);
                continue;
            }

            if (strcmp(workspace, normalized_workspace) != 0) {
                char *key_copy = malloc(key_len + 1);
                if (key_copy) {
                    memcpy(key_copy, key, key_len);
                    key_copy[key_len] = '\0';
                    if (put_value_to_leveldb(db, key_copy, normalized_workspace, strlen(normalized_workspace)) != 0) {
                        fprintf(stderr, "Failed to normalize stored workspace path for repo %d\n", repoid);
                    }
                    free(key_copy);
                }
            }

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

    platform_state_run_loop(platform_state);

    platform_state_destroy(platform_state);
    platform_state = NULL;
    // cleanup (not usually reached)
    return 0;
}
