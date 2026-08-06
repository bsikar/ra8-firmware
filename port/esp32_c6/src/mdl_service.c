/**
 * @file mdl_service.c
 * @brief ESP32-C6 side service for media downloading via RPC.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>

/* TODO: Include esp-idf HTTP client */
/* TODO: Include protobuf / rpc definitions */

void mdl_service_handle_request(void* request);
void mdl_service_task(void* pvParameters);

/**
 * @brief Stub for handling the ra8_mdl_download_req_t RPC message.
 *
 * @param[in] request Pointer to the incoming request (unused stub).
 */
void mdl_service_handle_request(void* request)
{
  (void)request;
  /* TODO: Parse ra8_mdl_download_req_t RPC message */
  /* TODO: Use arena allocator (ra8_arena_t) for all dynamic allocation */
  /* TODO: Fetch content using ESP-IDF HTTP client */
  /* TODO: Stream results back as ra8_mdl_download_progress_t */
}

/**
 * @brief Main task/stub for the media download service.
 *
 * @param[in] pvParameters Task parameters (unused stub).
 */
void mdl_service_task(void* pvParameters)
{
  (void)pvParameters;
  /* TODO: Listen for ra8_mdl_download_req_t RPC messages */
  for (;;) {
    /* TODO: Wait for RPC */
  }
}
