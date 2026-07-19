/**
 * @file test_ra8_sram_ecc.c
 * @brief ECC / runtime-control tests for ra8_sram.c (SRAM with ECC driver)
 *
 * @details
 * Split out of test_ra8_sram.c to keep each test translation unit under the
 * repository file-size cap. Drives the same host-side simulated SRAM control
 * window (``tests/mocks/ra8_sim_mmap.c``). This sibling owns the set_mode /
 * set_eccrgn / wait-state setters, status decode / clear, zero-init, ECC
 * self-test, bank info, security, boundary, dispatch, and MC/DC vector
 * tests; the init / deinit / stop contract tests stay in test_ra8_sram.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_err.h"
#include "ra8_mstp.h"
#include "ra8_sim_mmap.h"
#include "ra8_sram.h"
#include "ra8_sram_regs.h"
#include "unity_minimal.h"

/**
 * @enum sram_ecc_uint16_const_t
 * @brief Named uint16_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint16_t {
  k_sram_ecc_val_1000 = 0x1000U,
  k_sram_ecc_val_2000 = 0x2000U,
  k_sram_ecc_val_3000 = 0x3000U,
} sram_ecc_uint16_const_t;

/**
 * @enum sram_ecc_uint64_const_t
 * @brief Named uint64_t constants used by this file.
 *
 * @details
 * Every literal this translation unit needs, named so the
 * value's role is visible at the point of use (CLAUDE.md
 * "No Magic Numbers").
 */
typedef enum : uint64_t {
  k_sram_ecc_dst_deadbeefcafebabe = 0xDEADBEEFCAFEBABEULL,
} sram_ecc_uint64_const_t;

/* =============================================================================
 * Test constants
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra8_sram_test_bank_first = 0U, /**< RA8 SRAM test bank first. */
  k_ra8_sram_test_bank_one   = 1U, /**< RA8 SRAM test bank one.   */
  k_ra8_sram_test_bank_mid   = 2U, /**< RA8 SRAM test bank mid.   */
  k_ra8_sram_test_bank_last  = 3U, /**< RA8 SRAM test bank last.  */
  k_ra8_sram_test_bank_bad   = 4U, /**< RA8 SRAM test bank bad.   */
  k_ra8_sram_test_slot_bad   = 2U, /**< RA8 SRAM test slot bad.   */
} ra8_sram_test_bank_t;

typedef enum : uintptr_t {
  k_ra8_sram_test_ctx_token      = 0xC0FFEE12UL, /**< RA8 SRAM test ctx token.      */
  k_ra8_sram_test_bank_ctx_token = 0xBEEF0001UL, /**< RA8 SRAM test bank ctx token. */
} ra8_sram_test_ctx_t;

typedef enum : uintptr_t {
  k_ra8_sram_test_fault_addr = 0x12340U, /**< RA8 SRAM test fault address. */
} ra8_sram_test_addr_t;

typedef enum : uint32_t {
  k_ra8_sram_test_iclk_high  = 200000000UL, /**< 200 MHz -> wait needed.         */
  k_ra8_sram_test_iclk_low   = 100000000UL, /**< 100 MHz -> no wait needed.      */
  k_ra8_sram_test_iclk_max   = 250000000UL, /**< RA8D2 default max ICLK.         */
  k_ra8_sram_test_probe_off  = 0x100U,      /**< 8-byte aligned probe offset.    */
  k_ra8_sram_test_bank2_size = 0x80000UL,   /**< 512 KB SRAM2 size for boundary. */
  k_ra8_sram_test_sabar_off  = 0x10000UL,   /**< 64 KB Secure boundary, 4 KB OK. */
  k_ra8_sram_test_sabar_bad  = 0x10001UL,   /**< Mis-aligned (low bit set).      */
} ra8_sram_test_misc_t;

/* =============================================================================
 * Helpers / fixtures
 * =============================================================================
 */

static uint32_t  s_cb_count;
static uint8_t   s_cb_last_bank;
static bool      s_cb_last_2bit;
static uintptr_t s_cb_last_addr;
static void*     s_cb_last_ctx;

static uint32_t  s_bank_cb_count;
static uint8_t   s_bank_cb_last_bank;
static bool      s_bank_cb_last_2bit;
static uintptr_t s_bank_cb_last_addr;
static void*     s_bank_cb_last_ctx;

static void stub_error_cb(void* ctx, uint8_t bank, bool is_2bit, uintptr_t err_addr)
{
  ++s_cb_count;
  s_cb_last_bank = bank;
  s_cb_last_2bit = is_2bit;
  s_cb_last_addr = err_addr;
  s_cb_last_ctx  = ctx;
}

static void stub_bank_error_cb(void* ctx, uint8_t bank, bool is_2bit, uintptr_t err_addr)
{
  ++s_bank_cb_count;
  s_bank_cb_last_bank = bank;
  s_bank_cb_last_2bit = is_2bit;
  s_bank_cb_last_addr = err_addr;
  s_bank_cb_last_ctx  = ctx;
}

static void prep(void)
{
  ra8_sim_mmap_reset();
  (void)ra8_mstp_init();
  s_cb_count          = 0U;
  s_cb_last_bank      = 0U;
  s_cb_last_2bit      = false;
  s_cb_last_addr      = 0U;
  s_cb_last_ctx       = nullptr;
  s_bank_cb_count     = 0U;
  s_bank_cb_last_bank = 0U;
  s_bank_cb_last_2bit = false;
  s_bank_cb_last_addr = 0U;
  s_bank_cb_last_ctx  = nullptr;
}

static ra8_sram_config_t make_default_cfg(void)
{
  ra8_sram_config_t cfg = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_sram_bank_count; ++i) {
    cfg.banks[i].ecc_mode          = k_ra8_sram_ecc_disabled;
    cfg.banks[i].on_error          = k_ra8_sram_on_error_interrupt;
    cfg.banks[i].enable_1bit_latch = false;
    cfg.banks[i].eccrgn            = k_ra8_sram_region_off;
    cfg.banks[i].zero_init         = false;
  }
  cfg.apply_security = false;
  cfg.wait_state     = false;
  return cfg;
}

/* =============================================================================
 * Tests -- lifecycle
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

/* =============================================================================
 * Tests -- set_mode / set_eccrgn
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_mode_happy(void)
{
  TEST_BEGIN("sram set_mode reprograms one bank");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  const ra8_sram_bank_cfg_t bank_cfg = {
    .ecc_mode          = k_ra8_sram_ecc_with_chk,
    .on_error          = k_ra8_sram_on_error_reset,
    .enable_1bit_latch = true,
    .eccrgn            = k_ra8_sram_region_256kb,
    .zero_init         = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_mode((uint8_t)k_ra8_sram_test_bank_mid, &bank_cfg));

  volatile r_sram_regs_t* regs = ra8_sram_regs();
  const uint8_t           cr   = *ra8_sram_cr_ptr(regs, (uint8_t)k_ra8_sram_test_bank_mid);
  const uint8_t want = (uint8_t)k_ra8_sram_eccmod_with_chk | (uint8_t)k_ra8_sram_cr_mask_oad |
                       (uint8_t)k_ra8_sram_cr_mask_e1stsen;
  TEST_ASSERT_EQ(want, cr);
  TEST_ASSERT_EQ(k_ra8_sram_eccrgn_256kb,
                 *ra8_sram_eccrgn_ptr(regs, (uint8_t)k_ra8_sram_test_bank_mid));
  TEST_END("sram set_mode reprograms one bank");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mode_bad_bank(void)
{
  TEST_BEGIN("sram set_mode bad bank");
  prep();
  const ra8_sram_bank_cfg_t bank_cfg = {
    .ecc_mode          = k_ra8_sram_ecc_no_check,
    .on_error          = k_ra8_sram_on_error_interrupt,
    .enable_1bit_latch = false,
    .eccrgn            = k_ra8_sram_region_off,
    .zero_init         = false,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_mode((uint8_t)k_ra8_sram_test_bank_bad, &bank_cfg));
  TEST_END("sram set_mode bad bank");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_mode_null_cfg(void)
{
  TEST_BEGIN("sram set_mode null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sram_set_mode((uint8_t)k_ra8_sram_test_bank_first, nullptr));
  TEST_END("sram set_mode null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_eccrgn_happy(void)
{
  TEST_BEGIN("sram set_eccrgn updates SRAMECCRGNn only");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_set_eccrgn((uint8_t)k_ra8_sram_test_bank_one, k_ra8_sram_region_384kb));
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(k_ra8_sram_eccrgn_384kb,
                 *ra8_sram_eccrgn_ptr(regs, (uint8_t)k_ra8_sram_test_bank_one));
  TEST_END("sram set_eccrgn updates SRAMECCRGNn only");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_eccrgn_rejects_bank3_oversize(void)
{
  TEST_BEGIN("sram set_eccrgn bank3 over 128 KB rejected");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_eccrgn((uint8_t)k_ra8_sram_test_bank_last, k_ra8_sram_region_256kb));
  TEST_END("sram set_eccrgn bank3 over 128 KB rejected");
}

/* =============================================================================
 * Tests -- wait state
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_wait_state_manual(void)
{
  TEST_BEGIN("sram set_wait_state writes WTEN");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_wait_state(true));
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(k_ra8_sram_wtsc_wten, regs->SRAMWTSC);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_wait_state(false));
  TEST_ASSERT_EQ(0, regs->SRAMWTSC);
  TEST_END("sram set_wait_state writes WTEN");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_wait_state_for_clock(void)
{
  TEST_BEGIN("sram set_wait_state_for_clock follows HUM threshold");
  prep();

  /* Above half-max -> WTEN=1. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_set_wait_state_for_clock((uint32_t)k_ra8_sram_test_iclk_high,
                                                   (uint32_t)k_ra8_sram_test_iclk_max));
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(k_ra8_sram_wtsc_wten, regs->SRAMWTSC);

  /* Below half-max -> WTEN=0. */
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_set_wait_state_for_clock((uint32_t)k_ra8_sram_test_iclk_low,
                                                   (uint32_t)k_ra8_sram_test_iclk_max));
  TEST_ASSERT_EQ(0, regs->SRAMWTSC);

  /* Bad inputs rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_wait_state_for_clock(0U, (uint32_t)k_ra8_sram_test_iclk_max));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_wait_state_for_clock((uint32_t)k_ra8_sram_test_iclk_high, 0U));
  TEST_END("sram set_wait_state_for_clock follows HUM threshold");
}

/* =============================================================================
 * Tests -- status / clear
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_status_decode(void)
{
  TEST_BEGIN("sram status decodes per-bank flags");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* Inject error flags directly into the simulated SRAMESR. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR =
    (uint16_t)((uint16_t)k_ra8_sram_err_bank0_1bit | (uint16_t)k_ra8_sram_err_bank3_2bit);
  regs->SRAMEAR[k_ra8_sram_test_bank_first][0] = (uint32_t)k_ra8_sram_test_fault_addr;
  regs->SRAMEAR[k_ra8_sram_test_bank_last][1]  = (uint32_t)k_ra8_sram_test_fault_addr;

  ra8_sram_status_t out = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_get_status(&out));
  TEST_ASSERT_EQ(((uint16_t)k_ra8_sram_err_bank0_1bit | (uint16_t)k_ra8_sram_err_bank3_2bit),
                 out.raw_esr);
  TEST_ASSERT_EQ(0x01U, out.one_bit_mask); /* bank 0 only. */
  TEST_ASSERT_EQ(0x08U, out.two_bit_mask); /* bank 3 only. */
  /* Address is now absolute (offset + 0x2200_0000 base). */
  const uintptr_t want_abs =
    (uintptr_t)k_ra8_sram_data_base_addr + (uintptr_t)k_ra8_sram_test_fault_addr;
  TEST_ASSERT_EQ(want_abs, out.addr_1bit[k_ra8_sram_test_bank_first]);
  TEST_ASSERT_EQ(want_abs, out.addr_2bit[k_ra8_sram_test_bank_last]);
  TEST_END("sram status decodes per-bank flags");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_status_null_out(void)
{
  TEST_BEGIN("sram status null out");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sram_get_status(nullptr));
  TEST_END("sram status null out");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_writes_esclr(void)
{
  TEST_BEGIN("sram clear_status writes ESCLR");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_clear_status((uint16_t)k_ra8_sram_err_bank2_2bit));
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(k_ra8_sram_err_bank2_2bit, regs->SRAMESCLR);
  TEST_END("sram clear_status writes ESCLR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_status_rejects_reserved(void)
{
  TEST_BEGIN("sram clear_status rejects reserved bits");
  prep();
  /* Bits 8..15 are reserved in SRAMESCLR. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_clear_status((uint16_t)0x0100U));
  TEST_END("sram clear_status rejects reserved bits");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_clear_address_per_slot(void)
{
  TEST_BEGIN("sram clear_address writes single ESCLR bit");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_clear_address((uint8_t)k_ra8_sram_test_bank_one, (uint8_t)k_ra8_sram_ear_slot_2bit));
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(k_ra8_sram_err_bank1_2bit, regs->SRAMESCLR);

  /* Bad bank / slot rejected. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_clear_address((uint8_t)k_ra8_sram_test_bank_bad, (uint8_t)k_ra8_sram_ear_slot_1bit));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_clear_address((uint8_t)k_ra8_sram_test_bank_first, (uint8_t)k_ra8_sram_test_slot_bad));
  TEST_END("sram clear_address writes single ESCLR bit");
}

/* =============================================================================
 * Tests -- zero-init / self-test / introspection
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_zero_init_bank_writes_all_zero(void)
{
  TEST_BEGIN("sram zero_init_bank writes 64-bit zero across the bank");
  prep();
  /* Pre-fill SRAM3 with garbage so the zero pass has work to do.
   * SRAM3 is 128 KB which is the smallest bank -- fastest test. */
  volatile uint64_t* dst   = ra8_sram_bank_data_ptr((uint8_t)k_ra8_sram_test_bank_last);
  const uint32_t     bytes = ra8_sram_bank_size_bytes((uint8_t)k_ra8_sram_test_bank_last);
  const uint32_t     words = bytes >> (uint32_t)k_ra8_sram_ecc_word_shift;
  for (uint32_t i = 0U; i < words; ++i) {
    dst[i] = (uint64_t)k_sram_ecc_dst_deadbeefcafebabe;
  }

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_zero_init_bank((uint8_t)k_ra8_sram_test_bank_last));

  for (uint32_t i = 0U; i < words; ++i) {
    TEST_ASSERT_EQ(0, dst[i]);
  }

  /* Bank should be left in ECC-disabled mode. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  TEST_ASSERT_EQ(0, *ra8_sram_cr_ptr(regs, (uint8_t)k_ra8_sram_test_bank_last));

  /* Bad bank rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_zero_init_bank((uint8_t)k_ra8_sram_test_bank_bad));
  TEST_END("sram zero_init_bank writes 64-bit zero across the bank");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the true leg of the
 * single-condition caught decode `(esr & want_bit) != 0` by staging
 * the bank-1 1-bit SRAMESR latch the silicon ECC engine would set;
 * the RAM-backed host register file has no ECC engine)
 */
static void test_self_test_catches_1bit(void)
{
  TEST_BEGIN("sram self_test reports a staged 1-bit latch as caught");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* Stage the latch the silicon ECC engine would set for the verify
   * read (self_test itself never writes SRAMESR -- it is read-only). */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR                = (uint16_t)k_ra8_sram_err_bank1_1bit;

  bool caught = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_one,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    false,
                                    &caught));
  TEST_ASSERT(caught);

  /* The verify-step CR is 0x1C (ECC + check + 1-bit latch). */
  TEST_ASSERT_EQ(k_ra8_sram_cr_self_test_phase_verify,
                 *ra8_sram_cr_ptr(regs, (uint8_t)k_ra8_sram_test_bank_one));
  TEST_END("sram self_test reports a staged 1-bit latch as caught");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the true leg of the
 * caught decode for the 2-bit slot by staging the bank-1 2-bit latch)
 */
static void test_self_test_catches_2bit(void)
{
  TEST_BEGIN("sram self_test reports a staged 2-bit latch as caught");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR                = (uint16_t)k_ra8_sram_err_bank1_2bit;

  bool caught = false;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_one,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    true,
                                    &caught));
  TEST_ASSERT(caught);
  TEST_END("sram self_test reports a staged 2-bit latch as caught");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- drives the false leg of the
 * caught decode: no latch staged, then the WRONG slot staged. Both
 * cases must report not-caught, proving the decode selects the exact
 * (bank, slot) SRAMESR bit and does not fake success)
 */
static void test_self_test_miss_reports_not_caught(void)
{
  TEST_BEGIN("sram self_test reports not-caught without the expected latch");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* No latch staged at all: the ECC engine "missed" the fault. */
  bool caught = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_one,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    false,
                                    &caught));
  TEST_ASSERT(!caught);

  /* Wrong slot staged: a 1-bit latch must not satisfy a 2-bit probe. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR                = (uint16_t)k_ra8_sram_err_bank1_1bit;
  caught                       = true;
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_one,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    true,
                                    &caught));
  TEST_ASSERT(!caught);
  TEST_END("sram self_test reports not-caught without the expected latch");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_self_test_rejects_bad_offset(void)
{
  TEST_BEGIN("sram self_test rejects bad probe offset");
  prep();
  bool caught = false;
  /* Mis-aligned offset (low 3 bits set). */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_first, 7U, false, &caught));
  /* Beyond bank end. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_first, 0xFFFFFFF8U, false, &caught));
  /* Null caught pointer. */
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_first,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    false,
                                    nullptr));
  /* Bad bank. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_self_test((uint8_t)k_ra8_sram_test_bank_bad,
                                    (uint32_t)k_ra8_sram_test_probe_off,
                                    false,
                                    &caught));
  TEST_END("sram self_test rejects bad probe offset");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_get_bank_info(void)
{
  TEST_BEGIN("sram get_bank_info reports static layout");
  prep();
  ra8_sram_bank_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_get_bank_info((uint8_t)k_ra8_sram_test_bank_one, &info));
  TEST_ASSERT_EQ(k_ra8_sram_test_bank_one, info.bank);
  TEST_ASSERT_EQ(((uintptr_t)k_ra8_sram_data_base_addr + (uintptr_t)k_ra8_sram_bank1_data_off),
                 info.data_base);
  TEST_ASSERT_EQ(k_ra8_sram_bank012_size, info.data_size);
  TEST_ASSERT_EQ(((uintptr_t)k_ra8_sram_data_base_addr + (uintptr_t)k_ra8_sram_ecc_bank1_off),
                 info.ecc_base);
  TEST_ASSERT_EQ(k_ra8_sram_ecc_bank012_sz, info.ecc_size);

  /* SRAM3 is the small bank. */
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_get_bank_info((uint8_t)k_ra8_sram_test_bank_last, &info));
  TEST_ASSERT_EQ(k_ra8_sram_bank3_size, info.data_size);
  TEST_ASSERT_EQ(k_ra8_sram_ecc_bank3_sz, info.ecc_size);

  /* Bad bank / null. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_get_bank_info((uint8_t)k_ra8_sram_test_bank_bad, &info));
  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 ra8_sram_get_bank_info((uint8_t)k_ra8_sram_test_bank_first, nullptr));
  TEST_END("sram get_bank_info reports static layout");
}

/* =============================================================================
 * Tests -- TrustZone security helpers
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_set_security(void)
{
  TEST_BEGIN("sram set_security writes SRAMSAR");
  prep();
  const uint32_t mask = (uint32_t)k_ra8_sram_sar_bit_sa0 | (uint32_t)k_ra8_sram_sar_bit_wtsa;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_security(mask));
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  TEST_ASSERT_EQ(mask, cpscu->SRAMSAR);

  /* Reserved bits rejected. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_set_security(0xFFFFFFFFU));
  TEST_END("sram set_security writes SRAMSAR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_ecc_security(void)
{
  TEST_BEGIN("sram set_ecc_security writes SRAMESAR");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_ecc_security(true));
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  TEST_ASSERT_EQ(k_ra8_sram_esar_bit_esa, cpscu->SRAMESAR);

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_set_ecc_security(false));
  TEST_ASSERT_EQ(0, cpscu->SRAMESAR);
  TEST_END("sram set_ecc_security writes SRAMESAR");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_set_boundary(void)
{
  TEST_BEGIN("sram set_boundary writes SRAMSABARn");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_set_boundary((uint8_t)k_ra8_sram_test_bank_mid, (uint32_t)k_ra8_sram_test_sabar_off));
  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  TEST_ASSERT_EQ(k_ra8_sram_test_sabar_off, cpscu->SRAMSABAR[k_ra8_sram_test_bank_mid]);

  /* Mis-aligned boundary rejected. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_set_boundary((uint8_t)k_ra8_sram_test_bank_mid, (uint32_t)k_ra8_sram_test_sabar_bad));
  /* Bad bank rejected. */
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_set_boundary((uint8_t)k_ra8_sram_test_bank_bad, (uint32_t)k_ra8_sram_test_sabar_off));
  TEST_END("sram set_boundary writes SRAMSABARn");
}

/* =============================================================================
 * Tests -- callback dispatch
 * =============================================================================
  *
  * @par MC/DC:
  * (no compound decisions in this test -- exercises the public-API
  * happy path / error-rejection contract; no `&&` or `||` in the
  * code under test that this case touches)
 */

static void test_attach_null_fn_rejected(void)
{
  TEST_BEGIN("sram attach rejects null fn");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sram_attach_handler(nullptr, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_null_ptr,
    ra8_sram_attach_bank_handler((uint8_t)k_ra8_sram_test_bank_first, nullptr, nullptr));
  TEST_ASSERT_EQ(
    k_ra8_err_invalid_arg,
    ra8_sram_attach_bank_handler((uint8_t)k_ra8_sram_test_bank_bad, stub_bank_error_cb, nullptr));
  TEST_END("sram attach rejects null fn");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_fires_callback(void)
{
  TEST_BEGIN("sram dispatch fires global + bank callbacks");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_attach_handler(stub_error_cb, (void*)(uintptr_t)k_ra8_sram_test_ctx_token));
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_attach_bank_handler((uint8_t)k_ra8_sram_test_bank_mid,
                                              stub_bank_error_cb,
                                              (void*)(uintptr_t)k_ra8_sram_test_bank_ctx_token));

  ra8_sram_dispatch((uint8_t)k_ra8_sram_test_bank_mid, true, (uintptr_t)k_ra8_sram_test_fault_addr);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(k_ra8_sram_test_bank_mid, s_cb_last_bank);
  TEST_ASSERT(s_cb_last_2bit);
  TEST_ASSERT_EQ(k_ra8_sram_test_fault_addr, s_cb_last_addr);
  TEST_ASSERT_EQ(k_ra8_sram_test_ctx_token, (uintptr_t)s_cb_last_ctx);

  /* Per-bank handler also fired with the bank-specific ctx. */
  TEST_ASSERT_EQ(1, s_bank_cb_count);
  TEST_ASSERT_EQ(k_ra8_sram_test_bank_mid, s_bank_cb_last_bank);
  TEST_ASSERT(s_bank_cb_last_2bit);
  TEST_ASSERT_EQ(k_ra8_sram_test_bank_ctx_token, (uintptr_t)s_bank_cb_last_ctx);

  /* Out-of-range bank should be ignored. */
  ra8_sram_dispatch((uint8_t)k_ra8_sram_test_bank_bad,
                    false,
                    (uintptr_t)k_ra8_sram_test_fault_addr);
  TEST_ASSERT_EQ(1, s_cb_count);
  TEST_ASSERT_EQ(1, s_bank_cb_count);
  TEST_END("sram dispatch fires global + bank callbacks");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_dispatch_from_esr_walks_all_bits(void)
{
  TEST_BEGIN("sram dispatch_from_esr fans every set bit");
  prep();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    ra8_sram_attach_handler(stub_error_cb, (void*)(uintptr_t)k_ra8_sram_test_ctx_token));

  /* Forge SRAMESR with three flags set across two banks. */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  regs->SRAMESR =
    (uint16_t)((uint16_t)k_ra8_sram_err_bank0_1bit | (uint16_t)k_ra8_sram_err_bank2_2bit |
               (uint16_t)k_ra8_sram_err_bank3_1bit);
  regs->SRAMEAR[k_ra8_sram_test_bank_first][0] = k_sram_ecc_val_1000;
  regs->SRAMEAR[k_ra8_sram_test_bank_mid][1]   = k_sram_ecc_val_2000;
  regs->SRAMEAR[k_ra8_sram_test_bank_last][0]  = k_sram_ecc_val_3000;

  ra8_sram_status_t snapshot = {};
  const uint16_t    fired    = ra8_sram_dispatch_from_esr(&snapshot);

  TEST_ASSERT_EQ(((uint16_t)k_ra8_sram_err_bank0_1bit | (uint16_t)k_ra8_sram_err_bank2_2bit |
                  (uint16_t)k_ra8_sram_err_bank3_1bit),
                 fired);
  TEST_ASSERT_EQ(3, s_cb_count);
  TEST_ASSERT_EQ(snapshot.raw_esr, fired);
  TEST_END("sram dispatch_from_esr fans every set bit");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

/**
 * @test test_mcdc_ra8_sram
 *
 * @par MC/DC:
 * Decision A: ``ra8_sram_set_wait_state_for_clock`` line 582,
 * libs/ra8_hal/src/ra8_sram.c:
 * ``if ((iclk_hz == 0U) || (iclk_max_hz == 0U))`` (2 conditions, ``||``).
 * N+1 = 3:
 * - V1: iclk=200M, max=250M  -> dec F (compute wait)
 * - V2: iclk=0,    max=250M  -> dec T (invalid_arg)
 * - V3: iclk=200M, max=0     -> dec T (invalid_arg)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra8_sram(void)
{
  TEST_BEGIN("sram MC/DC: set_wait_state_for_clock 2-cond decision");
  prep();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8_sram_set_wait_state_for_clock((uint32_t)k_ra8_sram_test_iclk_high,
                                                   (uint32_t)k_ra8_sram_test_iclk_max));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_wait_state_for_clock(0U, (uint32_t)k_ra8_sram_test_iclk_max));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_sram_set_wait_state_for_clock((uint32_t)k_ra8_sram_test_iclk_high, 0U));
  TEST_END("sram MC/DC: set_wait_state_for_clock 2-cond decision");
}

/**
 * @var s_test_roster
 * @brief Fixed-order roster of every test case in this translation unit.
 *
 * @details
 * main() walks this table instead of naming each case, so its size does not
 * grow with the number of tests and adding a case is a one-line edit.
 *
 * @note Order is significant: cases run top to bottom, exactly as before.
 */
static void (*const s_test_roster[])(void) = {
  test_set_mode_happy,
  test_set_mode_bad_bank,
  test_set_mode_null_cfg,
  test_set_eccrgn_happy,
  test_set_eccrgn_rejects_bank3_oversize,
  test_set_wait_state_manual,
  test_set_wait_state_for_clock,
  test_status_decode,
  test_status_null_out,
  test_clear_status_writes_esclr,
  test_clear_status_rejects_reserved,
  test_clear_address_per_slot,
  test_zero_init_bank_writes_all_zero,
  test_self_test_catches_1bit,
  test_self_test_catches_2bit,
  test_self_test_miss_reports_not_caught,
  test_self_test_rejects_bad_offset,
  test_get_bank_info,
  test_set_security,
  test_set_ecc_security,
  test_set_boundary,
  test_attach_null_fn_rejected,
  test_dispatch_fires_callback,
  test_dispatch_from_esr_walks_all_bits,
  test_mcdc_ra8_sram,
};

int32_t main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  (void)fprintf(stderr, "[OK  ] test_ra8_sram_ecc.c\n");
  return 0;
}
