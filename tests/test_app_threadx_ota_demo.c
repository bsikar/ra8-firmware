/**
 * @file test_app_threadx_ota_demo.c
 * @brief Integration test: OTA orchestration smoke flow for the demo
 *
 * @details
 * The production app at examples/ek_ra8d2/threadx_ota_demo/main.c
 * spawns a single ThreadX worker that drives ra_ota_init -> a chain of
 * ra_ota_run_step calls -> ra_ota_commit_and_reboot through stub
 * net/crypto/flash backends. ThreadX is not in the host test build, so
 * this test exercises the same ra_ota orchestration surface the worker
 * thread calls, with locally-defined stub backends.
 *
 * This is a smoke test for the wiring -- detailed pipeline coverage
 * lives in tests/test_ra_ota.c.
 *
 * Exercised modules:
 *   - ra_ota            (init / deinit / precondition guards)
 *   - ra_board_ek_ra8d2 (LED1 + LED2 -- heartbeat + step beacon)
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_err.h"
#include "ra_ota.h"
#include "ra_pin_validator.h"
#include "ra_sim_mmap.h"
#include "unity_minimal.h"

/** @brief Per-test enums. */
typedef enum : uint32_t {
  k_test_ota_demo_bank_addr  = 0x02080000UL,
  k_test_ota_demo_bank_size  = 4096U,
  k_test_ota_demo_bank_index = 1U,
  k_test_ota_demo_pubkey     = 0x1234U,
} test_ota_demo_const_t;

/* -------------------------------------------------------------------------
 * Minimal stub backends -- mirror the demo's stub surface.
 * ------------------------------------------------------------------------- */

static ra_err_t stub_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)ctx;
  (void)url;
  if (out_len == NULL) {
    return k_ra_err_null_ptr;
  }
  *out_len = 0U;
  return k_ra_err_hw_error;
}

static ra_err_t stub_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out)
{
  (void)ctx;
  (void)dst;
  (void)cap;
  if (out != NULL) {
    *out = 0U;
  }
  return k_ra_err_hw_error;
}

static ra_err_t stub_net_close(void* ctx)
{
  (void)ctx;
  return k_ra_ok;
}

static ra_err_t stub_sha_init(void* ctx)
{
  (void)ctx;
  return k_ra_ok;
}

static ra_err_t stub_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  (void)ctx;
  (void)data;
  (void)len;
  return k_ra_ok;
}

static ra_err_t stub_sha_final(void* ctx, uint8_t out[k_ra_ota_sha256_bytes])
{
  (void)ctx;
  if (out == NULL) {
    return k_ra_err_null_ptr;
  }
  (void)memset(out, 0, k_ra_ota_sha256_bytes);
  return k_ra_ok;
}

static ra_err_t stub_ecdsa_verify(void*          ctx,
                                  uint32_t       pubkey_handle,
                                  const uint8_t  digest[k_ra_ota_sha256_bytes],
                                  const uint8_t* sig,
                                  uint32_t       sig_len)
{
  (void)ctx;
  (void)pubkey_handle;
  (void)digest;
  (void)sig;
  (void)sig_len;
  return k_ra_ok;
}

static ra_err_t stub_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)len;
  return k_ra_ok;
}

static ra_err_t stub_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  (void)addr;
  (void)src;
  (void)len;
  return k_ra_ok;
}

static ra_err_t stub_flash_set_startup(void* ctx, uint8_t which, bool persistent)
{
  (void)ctx;
  (void)which;
  (void)persistent;
  return k_ra_ok;
}

static ra_err_t stub_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  (void)addr;
  if ((dst != NULL) && (len > 0U)) {
    (void)memset(dst, 0, len);
  }
  return k_ra_ok;
}

/**
 * @brief Build a fully-populated cfg matching the demo's stub wiring.
 */
static ra_ota_cfg_t make_demo_cfg(void)
{
  ra_ota_cfg_t cfg = {};
  (void)snprintf(cfg.manifest_url, k_ra_ota_url_max_bytes, "https://example.test/manifest.json");
  cfg.pubkey_handle             = (uint32_t)k_test_ota_demo_pubkey;
  cfg.on_progress               = NULL;
  cfg.run_as_thread             = false;
  cfg.net.open                  = stub_net_open;
  cfg.net.read                  = stub_net_read;
  cfg.net.close                 = stub_net_close;
  cfg.crypto.sha256_init        = stub_sha_init;
  cfg.crypto.sha256_update      = stub_sha_update;
  cfg.crypto.sha256_final       = stub_sha_final;
  cfg.crypto.ecdsa_verify       = stub_ecdsa_verify;
  cfg.flash.erase               = stub_flash_erase;
  cfg.flash.program             = stub_flash_program;
  cfg.flash.set_startup         = stub_flash_set_startup;
  cfg.flash.readback            = stub_flash_readback;
  cfg.flash.inactive_bank_addr  = (uint32_t)k_test_ota_demo_bank_addr;
  cfg.flash.bank_size_bytes     = (uint32_t)k_test_ota_demo_bank_size;
  cfg.flash.inactive_bank_index = (uint8_t)k_test_ota_demo_bank_index;
  return cfg;
}

/**
 * @brief Per-test fixture reset.
 */
static void reset_world(void)
{
  ra_sim_mmap_reset();
  ra_pin_validator_reset();
  (void)ra_ota_deinit();
}

/**
 * @brief Init OTA module with the demo's stub surface.
 *
 * @par MC/DC:
 * Compound decision under test (in ra_ota_init): ``cfg == NULL ||
 * any required fn-ptr == NULL || URL empty || bank_size == 0``.
 * Four atomic conditions x N+1 = 5 vectors. This test covers the
 * all-valid vector; the failure vectors are split below.
 */
static void test_ota_demo_init_ok(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: ra_ota_init with stub backends");
  const ra_ota_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ota_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ota_deinit());
  TEST_END("ota_demo: ra_ota_init with stub backends");
}

/**
 * @brief NULL cfg rejected at init.
 *
 * @par MC/DC:
 * Decision vector under test: ``cfg == NULL`` failure path. Covers
 * the cfg-null vector of the init compound check.
 */
static void test_ota_demo_init_rejects_null_cfg(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: init rejects NULL cfg");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_ota_init(NULL));
  TEST_END("ota_demo: init rejects NULL cfg");
}

/**
 * @brief Init rejects partial backend (missing net.read fn-ptr).
 *
 * @par MC/DC:
 * Decision vector under test: ``net.read == NULL`` failure path inside
 * the init compound check.
 */
static void test_ota_demo_init_rejects_partial_net(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: init rejects net.read == NULL");
  ra_ota_cfg_t cfg = make_demo_cfg();
  cfg.net.read     = NULL;
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr, (int)ra_ota_init(&cfg));
  TEST_END("ota_demo: init rejects net.read == NULL");
}

/**
 * @brief Init rejects bank_size == 0 (the most common config bug).
 *
 * @par MC/DC:
 * Decision vector under test: ``bank_size == 0`` failure path inside
 * the init compound check.
 */
static void test_ota_demo_init_rejects_zero_bank(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: init rejects bank_size == 0");
  ra_ota_cfg_t cfg          = make_demo_cfg();
  cfg.flash.bank_size_bytes = 0U;
  TEST_ASSERT_EQ((int)k_ra_err_invalid_arg, (int)ra_ota_init(&cfg));
  TEST_END("ota_demo: init rejects bank_size == 0");
}

/**
 * @brief Double-init is rejected (worker thread can't re-spawn itself).
 *
 * @par MC/DC:
 * Decision vector under test: state-machine guard ``state != idle``
 * inside ra_ota_init. Pairs with the ok-init vector.
 */
static void test_ota_demo_init_rejects_double(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: double-init rejected");
  const ra_ota_cfg_t cfg = make_demo_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ota_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_err_invalid_state, (int)ra_ota_init(&cfg));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ota_deinit());
  TEST_END("ota_demo: double-init rejected");
}

/**
 * @brief LED beacons (LED1 + LED2) -- heartbeat + step indicators.
 *
 * @par MC/DC: not applicable -- sequential init/toggle of the two
 * status LEDs the demo uses.
 */
static void test_ota_demo_status_leds(void)
{
  reset_world();
  TEST_BEGIN("ota_demo: LED1 + LED2 init + toggle");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led1));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_init(k_ra_board_led2));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_toggle(k_ra_board_led1));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_board_led_toggle(k_ra_board_led2));
  TEST_END("ota_demo: LED1 + LED2 init + toggle");
}

int main(void)
{
  test_ota_demo_init_ok();
  test_ota_demo_init_rejects_null_cfg();
  test_ota_demo_init_rejects_partial_net();
  test_ota_demo_init_rejects_zero_bank();
  test_ota_demo_init_rejects_double();
  test_ota_demo_status_leds();
  return 0;
}
