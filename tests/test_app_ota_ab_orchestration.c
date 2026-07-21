/**
 * @file test_app_ota_ab_orchestration.c
 * @brief Integration test: A/B OTA orchestration demo logic + ra8_ota flow.
 *
 * @details
 * Mirrors
 * examples/ek_ra8d2/hw_pending/ota_ab_orchestration/main.c. The
 * ra8_ota state machine itself is owned + covered by its own unit tests
 * (test_ra8_ota*.c); this test pins down the app-level contract the demo relies
 * on:
 *
 *  - The A/B outcome classifier ``app_ab_classify`` and the final verdict
 *    ``app_ab_ok`` with full MC/DC (mirrored as pure functions, same shape as
 *    the demo).
 *  - The end-to-end flow the demo drives against injected backends: a good image
 *    streamed into a RAM "bank" verifies and COMMITS (state ``done``,
 *    ``set_startup`` latches the inactive bank); a corrupted image fails the
 *    real SHA-256 integrity re-hash and ROLLS BACK (state ``error``,
 *    ``set_startup`` never called), exactly the two paths the SIL gate asserts.
 *
 * The crypto backend is the real software SHA-256 (``ra8_rsip_sha256*``), so the
 * integrity check that triggers the rollback is exercised for real, host-side.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_ota.h"
#include "ra8_rsip.h"
#include "unity_minimal.h"

/* ==========================================================================
 * Mirror of the demo's pure A/B decision logic (kept in step with main.c).
 * ========================================================================== */

/** @brief Mirror of the demo's ::app_outcome_t. */
typedef enum : uint8_t {
  k_t_outcome_committed     = 0U, /**< Verify passed and the swap latched. */
  k_t_outcome_rolled_back   = 1U, /**< Verify failed; no swap.             */
  k_t_outcome_indeterminate = 2U, /**< Neither terminal shape matched.     */
} t_outcome_t;

/**
 * @brief Mirror of the demo's ``app_ab_classify``.
 *
 * @details ``(ok, done)`` = committed; ``(!=ok, error)`` = rolled back;
 * anything else indeterminate. Two compound AND decisions.
 */
static t_outcome_t t_classify(ra8_err_t result, ra8_ota_state_t state)
{
  if ((result == k_ra8_ok) && (state == k_ra8_ota_state_done)) {
    return k_t_outcome_committed;
  }
  if ((result != k_ra8_ok) && (state == k_ra8_ota_state_error)) {
    return k_t_outcome_rolled_back;
  }
  return k_t_outcome_indeterminate;
}

/**
 * @brief Mirror of the demo's ``app_ab_ok`` verdict.
 *
 * @details ``ok = committed && rolled_back`` -- both A/B paths behaved.
 */
static bool t_ok(bool committed, bool rolled_back)
{
  return committed && rolled_back;
}

/* ==========================================================================
 * Demo-mirrored backends for the end-to-end flow.
 * ========================================================================== */

/** @brief Sizing + demo constants shared with the app. */
typedef enum : uint32_t {
  k_t_image_bytes = 256U,        /**< Demo image size.                      */
  k_t_bank_size   = 4096U,       /**< Advertised bank capacity for the cfg. */
  k_t_demo_pubkey = 0x0A8D2C0DU, /**< Demo public-key handle.               */
} t_const_t;

/** @brief A/B slot indices. */
typedef enum : uint8_t {
  k_t_bank_a = 0U, /**< Active slot.   */
  k_t_bank_b = 1U, /**< Inactive slot. */
} t_bank_t;

/** @brief Demo signature width. */
typedef enum : uint8_t {
  k_t_sig_bytes = 32U, /**< Demo authenticity tag length. */
} t_sig_t;

/** @brief Demo image byte-pattern + tag constants (mirror of the app). */
typedef enum : uint8_t {
  k_t_img_seed_mul  = 7U,    /**< Golden-image pattern multiplier.    */
  k_t_img_seed_add  = 3U,    /**< Golden-image pattern additive bias. */
  k_t_sig_tag_base  = 0xA5U, /**< Demo signature tag base byte.       */
  k_t_byte_all_ones = 0xFFU, /**< Corrupt-flip mask / erased byte.    */
} t_pattern_t;

/** @brief In-RAM mock of the inactive bank + boot-select state. */
typedef struct {
  uint8_t bank[k_t_image_bytes]; /**< Programmed bank bytes.              */
  uint8_t bootsel_bank;          /**< Bank named by the last set_startup. */
  bool    set_startup_called;    /**< Whether commit latched a swap.      */
} t_flash_t;

/** @brief In-RAM "download" cursor. */
typedef struct {
  const uint8_t* data; /**< Image bytes served.        */
  uint32_t       len;  /**< Total image length.        */
  uint32_t       pos;  /**< Bytes already handed over. */
} t_net_t;

static t_flash_t             s_flash;
static t_net_t               s_net;
static ra8_rsip_sha256_ctx_t s_sha_ctx;
static uint8_t               s_image_good[k_t_image_bytes];
static uint8_t               s_image_bad[k_t_image_bytes];
static uint8_t               s_good_digest[k_ra8_ota_sha256_bytes];

static ra8_err_t t_net_open(void* ctx, const char* url, uint32_t* out_len)
{
  (void)url;
  t_net_t* n = (t_net_t*)ctx;
  n->pos     = 0U;
  *out_len   = n->len;
  return k_ra8_ok;
}

static ra8_err_t t_net_read(void* ctx, uint8_t* dst, uint32_t cap, uint32_t* out_len)
{
  t_net_t*       n   = (t_net_t*)ctx;
  const uint32_t rem = n->len - n->pos;
  const uint32_t k   = (rem < cap) ? rem : cap;
  if (k > 0U) {
    (void)memcpy(dst, &n->data[n->pos], k);
    n->pos += k;
  }
  *out_len = k;
  return k_ra8_ok;
}

static ra8_err_t t_net_close(void* ctx)
{
  (void)ctx;
  return k_ra8_ok;
}

static ra8_err_t t_sha_init(void* ctx)
{
  return ra8_rsip_sha256_init((ra8_rsip_sha256_ctx_t*)ctx);
}

static ra8_err_t t_sha_update(void* ctx, const uint8_t* data, uint32_t len)
{
  return ra8_rsip_sha256_update((ra8_rsip_sha256_ctx_t*)ctx, data, len);
}

static ra8_err_t t_sha_final(void* ctx, uint8_t out[k_ra8_ota_sha256_bytes])
{
  return ra8_rsip_sha256_final((ra8_rsip_sha256_ctx_t*)ctx, out);
}

static ra8_err_t t_ecdsa(void*          ctx,
                         uint32_t       pubkey,
                         const uint8_t  bound[k_ra8_ota_sha256_bytes],
                         const uint8_t* sig,
                         uint32_t       sig_len)
{
  (void)ctx;
  (void)bound;
  (void)sig;
  if ((pubkey != (uint32_t)k_t_demo_pubkey) || (sig_len != (uint32_t)k_t_sig_bytes)) {
    return k_ra8_err_hw_error;
  }
  return k_ra8_ok;
}

static ra8_err_t t_flash_erase(void* ctx, uint32_t addr, uint32_t len)
{
  (void)ctx;
  (void)memset(&s_flash.bank[addr], (int)k_t_byte_all_ones, (size_t)len);
  return k_ra8_ok;
}

static ra8_err_t t_flash_program(void* ctx, uint32_t addr, const uint8_t* src, uint32_t len)
{
  (void)ctx;
  (void)memcpy(&s_flash.bank[addr], src, (size_t)len);
  return k_ra8_ok;
}

static ra8_err_t t_flash_readback(void* ctx, uint32_t addr, uint8_t* dst, uint32_t len)
{
  (void)ctx;
  (void)memcpy(dst, &s_flash.bank[addr], (size_t)len);
  return k_ra8_ok;
}

static ra8_err_t t_flash_set_startup(void* ctx, uint8_t which_bank, bool persistent)
{
  (void)ctx;
  (void)persistent;
  s_flash.set_startup_called = true;
  s_flash.bootsel_bank       = which_bank;
  return k_ra8_ok;
}

static void t_make_cfg(ra8_ota_cfg_t* cfg)
{
  (void)memset(cfg, 0, sizeof *cfg);
  (void)memcpy(cfg->manifest_url, "local://demo", strlen("local://demo") + 1U);
  cfg->pubkey_handle             = (uint32_t)k_t_demo_pubkey;
  cfg->net.open                  = t_net_open;
  cfg->net.read                  = t_net_read;
  cfg->net.close                 = t_net_close;
  cfg->net.ctx                   = &s_net;
  cfg->crypto.sha256_init        = t_sha_init;
  cfg->crypto.sha256_update      = t_sha_update;
  cfg->crypto.sha256_final       = t_sha_final;
  cfg->crypto.ecdsa_verify       = t_ecdsa;
  cfg->crypto.ctx                = &s_sha_ctx;
  cfg->flash.erase               = t_flash_erase;
  cfg->flash.program             = t_flash_program;
  cfg->flash.set_startup         = t_flash_set_startup;
  cfg->flash.readback            = t_flash_readback;
  cfg->flash.inactive_bank_addr  = 0U;
  cfg->flash.bank_size_bytes     = (uint32_t)k_t_bank_size;
  cfg->flash.inactive_bank_index = (uint8_t)k_t_bank_b;
  cfg->flash.ctx                 = nullptr;
}

static void t_make_manifest(ra8_ota_manifest_t* m)
{
  (void)memset(m, 0, sizeof *m);
  (void)memcpy(m->version, "1.0.1", strlen("1.0.1") + 1U);
  (void)memcpy(m->image_url, "local://demo/app.bin", strlen("local://demo/app.bin") + 1U);
  m->image_size_bytes = (uint32_t)k_t_image_bytes;
  (void)memcpy(m->image_sha256, s_good_digest, sizeof m->image_sha256);
  for (uint8_t i = 0U; i < (uint8_t)k_t_sig_bytes; ++i) {
    m->signature[i] = (uint8_t)((uint8_t)k_t_sig_tag_base ^ i);
  }
  m->signature_len = (uint16_t)k_t_sig_bytes;
}

static void t_prep_images(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_t_image_bytes; ++i) {
    s_image_good[i] = (uint8_t)((i * (uint32_t)k_t_img_seed_mul) + (uint32_t)k_t_img_seed_add);
    s_image_bad[i]  = s_image_good[i];
  }
  s_image_bad[0] = (uint8_t)(s_image_good[0] ^ (uint8_t)k_t_byte_all_ones);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_rsip_sha256(s_image_good, (uint32_t)k_t_image_bytes, s_good_digest));
}

/** @brief Drive one full attempt against @p source; returns the classified outcome. */
static t_outcome_t t_run(const uint8_t* source, const ra8_ota_manifest_t* m)
{
  (void)memset(&s_flash, 0, sizeof s_flash);
  s_net.data = source;
  s_net.len  = (uint32_t)k_t_image_bytes;
  s_net.pos  = 0U;

  ra8_ota_cfg_t cfg;
  t_make_cfg(&cfg);
  (void)ra8_ota_deinit();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_ota_init(&cfg));

  ra8_err_t r = ra8_ota_download_to_inactive_bank(m);
  if (r == k_ra8_ok) {
    r = ra8_ota_verify_signature(m);
  }
  if (r == k_ra8_ok) {
    r = ra8_ota_commit_and_reboot();
  }
  const ra8_ota_state_t st = ra8_ota_get_state();
  (void)ra8_ota_deinit();
  return t_classify(r, st);
}

/* ==========================================================================
 * MC/DC unit tests for the pure decision logic.
 * ========================================================================== */

/**
 * @test app_ab_classify_mcdc
 *
 * @par MC/DC:
 * Decision D1 `(result == ok) && (state == done)` and
 * decision D2 `(result != ok) && (state == error)`, 2 conditions each. Vectors:
 * - V1: (ok,  done)  -> committed      (D1 both true)
 * - V2: (err, error) -> rolled_back    (D2 both true)
 * - V3: (ok,  error) -> indeterminate  (D1 varies state vs V1; D2 varies result vs V2)
 * - V4: (err, done)  -> indeterminate  (D1 varies result vs V1; D2 varies state vs V2)
 * V1+V4 prove result independence for D1; V1+V3 prove state independence for D1.
 * V2+V3 prove result independence for D2; V2+V4 prove state independence for D2.
 */
static void test_classify_mcdc(void)
{
  TEST_BEGIN("ota_ab: classify MC/DC");
  TEST_ASSERT_EQ(k_t_outcome_committed, t_classify(k_ra8_ok, k_ra8_ota_state_done)); /* V1 */
  TEST_ASSERT_EQ(k_t_outcome_rolled_back,
                 t_classify(k_ra8_err_crc_mismatch, k_ra8_ota_state_error));              /* V2 */
  TEST_ASSERT_EQ(k_t_outcome_indeterminate, t_classify(k_ra8_ok, k_ra8_ota_state_error)); /* V3 */
  TEST_ASSERT_EQ(k_t_outcome_indeterminate,
                 t_classify(k_ra8_err_crc_mismatch, k_ra8_ota_state_done)); /* V4 */
  TEST_END("ota_ab: classify MC/DC");
}

/**
 * @test app_ab_ok_mcdc
 *
 * @par MC/DC:
 * Decision `committed && rolled_back` (2 conditions). N+1 = 3 vectors:
 * - committed=1, rolled_back=1 -> 1  (control)
 * - committed=0, rolled_back=1 -> 0  (varies committed)
 * - committed=1, rolled_back=0 -> 0  (varies rolled_back)
 */
static void test_ok_mcdc(void)
{
  TEST_BEGIN("ota_ab: verdict MC/DC");
  TEST_ASSERT(t_ok(true, true));   /* control         */
  TEST_ASSERT(!t_ok(false, true)); /* vary committed  */
  TEST_ASSERT(!t_ok(true, false)); /* vary rolledback */
  TEST_END("ota_ab: verdict MC/DC");
}

/* ==========================================================================
 * End-to-end A/B flow tests (real SHA-256, mock RAM bank).
 * ========================================================================== */

/**
 * @test commit_path
 *
 * @par MC/DC:
 * No compound decision -- asserts the demo's COMMIT contract: a good image
 * verifies (real SHA-256 re-hash matches the manifest digest), the machine ends
 * in ``done``, and ``set_startup`` latched the inactive bank (B).
 */
static void test_commit_path(void)
{
  TEST_BEGIN("ota_ab: commit path");
  t_prep_images();
  ra8_ota_manifest_t m;
  t_make_manifest(&m);

  const t_outcome_t out = t_run(s_image_good, &m);
  TEST_ASSERT_EQ(k_t_outcome_committed, out);
  TEST_ASSERT(s_flash.set_startup_called);
  TEST_ASSERT_EQ(k_t_bank_b, s_flash.bootsel_bank);
  TEST_END("ota_ab: commit path");
}

/**
 * @test rollback_path
 *
 * @par MC/DC:
 * No compound decision -- asserts the demo's ROLLBACK contract: a corrupted
 * image re-hashes to a digest that does NOT match the manifest, so verify
 * returns a mismatch, the machine parks in ``error``, and ``set_startup`` is
 * never called (the boot bank is left on the good active bank A).
 */
static void test_rollback_path(void)
{
  TEST_BEGIN("ota_ab: rollback path");
  t_prep_images();
  ra8_ota_manifest_t m;
  t_make_manifest(&m);

  const t_outcome_t out = t_run(s_image_bad, &m);
  TEST_ASSERT_EQ(k_t_outcome_rolled_back, out);
  TEST_ASSERT(!s_flash.set_startup_called);
  TEST_END("ota_ab: rollback path");
}

/**
 * @test verdict_combines_both_paths
 *
 * @par MC/DC:
 * No compound decision -- confirms the demo's final ``ok`` is true only when the
 * good image committed AND the corrupted image rolled back, matching the SIL
 * banner ``ota_ab: ... ok=Y``.
 */
static void test_full_verdict(void)
{
  TEST_BEGIN("ota_ab: full verdict");
  t_prep_images();
  ra8_ota_manifest_t m;
  t_make_manifest(&m);

  const bool committed   = (t_run(s_image_good, &m) == k_t_outcome_committed);
  const bool rolled_back = (t_run(s_image_bad, &m) == k_t_outcome_rolled_back);
  TEST_ASSERT(t_ok(committed, rolled_back));
  TEST_END("ota_ab: full verdict");
}

int main(void)
{
  test_classify_mcdc();
  test_ok_mcdc();
  test_commit_path();
  test_rollback_path();
  test_full_verdict();
  return 0;
}
