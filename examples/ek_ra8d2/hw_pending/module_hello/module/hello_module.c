/**
 * @file hello_module.c
 * @brief A simple "Hello World" ThreadX module for RA8.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#define TXM_MODULE
#include "ra8_app_api.h"
#include "txm_module.h"

/**
 * @brief The entry point for the module.
 * @param id Module ID passed by the manager.
 */
void demo_module_start(ULONG id)
{
  (void)id;

  /* Call the info logger via SVC */
  txm_module_application_request(k_ra8_app_api_log_info, (ULONG) "Hello from module!", 0, 0);

  /* Request stop via SVC */
  txm_module_application_request(k_ra8_app_api_request_stop, 0, 0, 0);
}
