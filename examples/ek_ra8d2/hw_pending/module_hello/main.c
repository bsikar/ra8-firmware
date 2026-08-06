/**
 * @file main.c
 * @brief ThreadX Module Manager hello-world kernel-side example.
 * @ingroup grp_examples
 *
 * @details
 * Initialises the ThreadX Module Manager, loads a compiled-in module
 * binary in-place, and starts it. The module runs inside the MPU sandbox.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "tx_api.h"
#include "txm_module.h"
#include "ra8_log.h"

#include <stdint.h>

static const char s_tag[] = "module_hello";

/* ---- Embedded module binary ---------------------------------------------- */
/* Linked via objcopy --binary in CMakeLists.txt. */
extern const uint8_t _binary_hello_module_bin_start[];
extern const uint8_t _binary_hello_module_bin_end[];

/* ---- Module Manager memory pools ----------------------------------------- */
enum : uint32_t {
  k_mgr_pool_bytes = 64U * 1024U,
  k_obj_pool_bytes = 16U * 1024U,
};
static uint8_t s_mgr_pool[k_mgr_pool_bytes] __attribute__((aligned(4)));
static uint8_t s_obj_pool[k_obj_pool_bytes] __attribute__((aligned(4)));

/* ---- Module instance ----------------------------------------------------- */
static TXM_MODULE_INSTANCE s_hello_module;

/**
 * @brief ThreadX application entry — called from tx_kernel_enter().
 *
 * @param[in] first_unused_memory  First byte after kernel-owned RAM.
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  ra8_log_info(s_tag, "module_hello: starting Module Manager");

  /* 1. Initialise the Module Manager pool. */
  UINT st = txm_module_manager_initialize(s_mgr_pool, k_mgr_pool_bytes);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_initialize failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module manager initialized");

  /* 2. Create an object pool for module-created ThreadX objects. */
  st = txm_module_manager_object_pool_create(s_obj_pool, k_obj_pool_bytes);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_object_pool_create failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "object pool created");

  /* 3. Load the module in-place from the compiled-in binary. */
  const uint32_t mod_size = (uint32_t)(_binary_hello_module_bin_end -
                                       _binary_hello_module_bin_start);
  ra8_log_info_val(s_tag, "module binary size", mod_size);

  st = txm_module_manager_in_place_load(&s_hello_module, "hello",
                                        (VOID*)_binary_hello_module_bin_start);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_in_place_load failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module loaded");

  /* 4. Start the module. */
  st = txm_module_manager_start(&s_hello_module);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_start failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module started — running inside MPU sandbox");
}
