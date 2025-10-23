#ifndef FSEVENT_CORE_H
#define FSEVENT_CORE_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct fsevent_core_configuration {
    const char *database_path;
    const char *workspace_override;
    int enable_http_server;
} fsevent_core_configuration;

int fsevent_core_run(const fsevent_core_configuration *configuration);
int fsevent_core_run_default(void);
int fsevent_core_prepare_shared_storage(char *out, size_t out_size);

#ifdef __cplusplus
}
#endif

#endif
