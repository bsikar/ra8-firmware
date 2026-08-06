#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#pragma pack(push, 1)

typedef struct {
    char url[256];
    char dest_path[256];
    uint32_t chapter_info;
} ra8_mdl_download_req_t;

typedef struct {
    uint32_t bytes_received;
    uint32_t total_bytes;
    uint32_t status;
} ra8_mdl_download_progress_t;

typedef struct {
    uint32_t chunk_length;
    uint8_t data[1024];
} ra8_mdl_download_chunk_t;

typedef struct {
    uint8_t dummy; /* Empty struct is not allowed in standard C, using a dummy field */
} ra8_mdl_cancel_req_t;

#pragma pack(pop)

#ifdef __cplusplus
}
#endif
