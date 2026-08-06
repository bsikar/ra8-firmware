/**
 * @file main.c
 * @brief ThreadX Module Manager hello-world kernel-side example.
 * @ingroup grp_examples
 *
 * @details
 * Initialises the ThreadX Module Manager, verifies the Ed25519 signature
 * on a compiled-in module binary, loads it in-place, and starts it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "tx_api.h"
#include "txm_module.h"
#include "ra8_app_verify.h"
#include "ra8_app_api.h"
#include "ra8_err.h"
#include "ra8_log.h"

static const char s_tag[] = "module_hello";

/* ---- Dummy module binary (stub for build verification) ------------------- */
/* In a real app this would be loaded from SD card or linked via objcopy.
 * For build verification, provide a minimal stub. */
static const uint8_t s_dummy_module_bin[128] __attribute__((aligned(4))) = {0};

/* ---- Dummy Ed25519 public key (zeroed; real key goes here) --------------- */
static const uint8_t s_dummy_pubkey[32] = {0};

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

  /* 2. Create an object pool for module-created ThreadX objects. */
  st = txm_module_manager_object_pool_create(s_obj_pool, k_obj_pool_bytes);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_object_pool_create failed", (uint32_t)st);
    return;
  }

  /* 3. Verify the compiled-in module binary. */
  const ra8_err_t verify = ra8_app_verify(s_dummy_module_bin,
                                          (uint32_t)sizeof(s_dummy_module_bin),
                                          s_dummy_pubkey);
  if (verify != k_ra8_ok) {
    ra8_log_error_val(s_tag, "module signature check failed", (uint32_t)verify);
    /* Expected to fail with the dummy binary — this is fine for the
     * build verification PoC. In a real app, this would reject the
     * module and return. */
    ra8_log_info(s_tag, "module_hello: build verification PoC complete (dummy binary, expected signature failure)");
    return;
  }

  ra8_log_info(s_tag, "module signature OK");

  /* 4. Load the module in-place. */
  st = txm_module_manager_in_place_load(&s_hello_module, "hello",
                                        (VOID*)s_dummy_module_bin);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_in_place_load failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module loaded");

  /* 5. Start the module. */
  st = txm_module_manager_start(&s_hello_module);
  if (st != TX_SUCCESS) {
    ra8_log_error_val(s_tag, "txm_module_manager_start failed", (uint32_t)st);
    return;
  }
  ra8_log_info(s_tag, "module started");
}
