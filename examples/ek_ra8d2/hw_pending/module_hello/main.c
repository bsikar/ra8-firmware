/**
 * @file main.c
 * @brief ThreadX Module Manager hello-world kernel-side example.
 *
 * @details
 * Initialises the ThreadX Module Manager, loads a compiled-in module
 * binary in-place, and starts it. The module runs inside the MPU sandbox.
 *
 * Module start must happen from a running thread (not tx_application_define)
 * because txm_module_manager_start acquires a mutex with TX_WAIT_FOREVER.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_log.h"
#include "tx_api.h"
#include "txm_module.h"

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

/* ---- Module binary aligned copy buffer ----------------------------------- */
static uint8_t s_mod_aligned[2048U] __attribute__((aligned(32)));

/* ---- Module instance ----------------------------------------------------- */
static TXM_MODULE_INSTANCE s_hello_module;

/* ---- Startup thread ------------------------------------------------------ */
static TX_THREAD s_startup_thread;
static uint8_t   s_startup_stack[2048U] __attribute__((aligned(4)));

/**
 * @brief Startup thread - loads and starts the module.
 *
 * @details
 * txm_module_manager_start acquires a mutex with TX_WAIT_FOREVER, so it
 * must be called from a running thread, not from tx_application_define.
 */
static void startup_thread_entry(ULONG input)
{
  (void)input;

  /* Copy module binary into aligned buffer. */
  const uint32_t mod_size =
    (uint32_t)((uintptr_t)_binary_hello_module_bin_end - (uintptr_t)_binary_hello_module_bin_start);
  ra8_log_info_val(s_tag, "module binary size", mod_size);

  if (mod_size > sizeof(s_mod_aligned)) {
    ra8_log_error_val(s_tag, "module too large", mod_size);
    return;
  }
  __builtin_memcpy(s_mod_aligned, _binary_hello_module_bin_start, mod_size);

  /* Debug: check the preamble raw bytes in s_mod_aligned. */
  uint32_t* preamble = (uint32_t*)s_mod_aligned;
  ra8_log_error_val(s_tag, "preamble[0] (id)", preamble[0]);
  ra8_log_error_val(s_tag, "preamble[10] (start_stack)", preamble[10]);
  ra8_log_error_val(s_tag, "preamble[13] (cb_stack)", preamble[13]);

  /* Load the module in-place. */
  UINT st = txm_module_manager_in_place_load(&s_hello_module, "hello", (VOID*)s_mod_aligned);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "in_place_load failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module loaded");

  /* Start the module. */
  st = txm_module_manager_start(&s_hello_module);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "module_manager_start failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module started - running inside MPU sandbox");
}

/**
 * @brief ThreadX application entry - called from tx_kernel_enter().
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

  /* 3. Create a startup thread to load and start the module.
   * Must be a thread because txm_module_manager_start uses a mutex. */
  st = tx_thread_create(&s_startup_thread,
                        "startup",
                        startup_thread_entry,
                        0U,
                        s_startup_stack,
                        sizeof(s_startup_stack),
                        1U,
                        1U, /* Priority 1 (high) */
                        TX_NO_TIME_SLICE,
                        TX_AUTO_START);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "tx_thread_create startup failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "startup thread created - will load module");
}
