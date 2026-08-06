/**
 * @file mdl_service.c
 * @brief ESP32-C6 side service for media downloading via RPC.
 */

#include <stdint.h>
#include <stddef.h>

// TODO: Include esp-idf HTTP client
// TODO: Include protobuf / rpc definitions
// #include "ra8_arena.h"

/**
 * @brief Stub for handling the MdlDownloadRequest RPC message.
 */
void mdl_service_handle_request(void* request) {
    // TODO: Parse MdlDownloadRequest RPC message
    // TODO: Use arena allocator (ra8_arena_t) for all dynamic allocation
    // TODO: Fetch content using ESP-IDF HTTP client
    // TODO: Stream results back as MdlDownloadProgress/MdlDownloadResult
}

/**
 * @brief Main task/stub for the media download service.
 */
void mdl_service_task(void* pvParameters) {
    // TODO: Listen for MdlDownloadRequest RPC messages
    while (1) {
        // TODO: Wait for RPC
    }
}
