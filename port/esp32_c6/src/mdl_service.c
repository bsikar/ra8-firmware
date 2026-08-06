/**
 * @file mdl_service.c
 * @brief ESP32-C6 side service for media downloading via RPC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

// TODO: Include esp-idf HTTP client
// TODO: Include protobuf / rpc definitions
// #include "ra8_arena.h"

/**
void mdl_service_handle_request(void* request);
void mdl_service_task(void* pvParameters);

/**
 * @brief Stub for handling the ra8_mdl_download_req_t RPC message.
 */
void mdl_service_handle_request(void* request)
{
  (void)request;
  // TODO: Parse ra8_mdl_download_req_t RPC message
  // TODO: Use arena allocator (ra8_arena_t) for all dynamic allocation
  // TODO: Fetch content using ESP-IDF HTTP client
  // TODO: Stream results back as ra8_mdl_download_progress_t/ra8_mdl_download_result_t
}

/**
 * @brief Main task/stub for the media download service.
 */
void mdl_service_task(void* pvParameters)
{
  (void)pvParameters;
  // TODO: Listen for ra8_mdl_download_req_t RPC messages
  while (1) {
    // TODO: Wait for RPC
  }
}
