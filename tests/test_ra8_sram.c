/**
 * @file test_ra8_sram.c
 * @brief Unit tests for ra8_sram.c (SRAM with ECC driver)
 *
 * @details
 * This sibling owns the init / deinit / stop contract tests; the setter,
 * status, self-test, dispatch, and MC/DC vector tests live in
 * test_ra8_sram_ecc.c.
 *
 * Drives the host-side fake SRAM control window provided by
 * ``tests/mocks/ra8_fake_mmap.c``. Both the SRAM control block at
 * ``0x4000_2000`` and the SRAM data alias at ``0x2200_0000`` (2 MiB)
 * are mapped for tests, so the zero-init pass and the ECC self-test
 * exercise real memory.
 *
 * Coverage now matches the round-3 brief:
 *  - happy init applies SRAMCRn under SRAMPRCR_S unlock
 *  - null-cfg / out-of-range bank / out-of-range region rejected
 *  - ``ra8_sram_set_mode`` reprograms a single bank's CR + ECCRGN
 *  - ``ra8_sram_set_eccrgn`` standalone, plus SRAM3 limit
 *  - status get / clear round-trip including EAR auto-clear
 *  - clear_address per (bank, slot)
 *  - global + per-bank error callback dispatch
 *  - ``ra8_sram_dispatch_from_esr`` walks every set ESR bit
 *  - zero-init pass writes all-zero across the bank
 *  - ECC decoder self-test decodes staged SRAMESR latches (hit + miss;
 *    the RAM-backed host register file has no ECC engine, so tests
 *    stage the latch the silicon engine would set)
 *  - SRAMSAR / SRAMESAR / SRAMSABARn writes via the security helpers
 *  - SRAMWTSC manual + auto-from-clock helpers
 *  - enter_stop / exit_stop lifecycle
 *  - bank-info introspection
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>
#include <string.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_sram.h"
#include "ra8_sram_regs.h"
#include "unity_minimal.h"

/**
 * @enum t_sram_t
 * @brief Out-of-range ECC mode the bank validator must reject.
 */
typedef enum : uint8_t {
  k_t_ecc_mode_bad = 0xFFU, /**< Past the last defined ECC mode. */
} t_sram_t;

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

static void prep(void)
{
  ra8_fake_mmap_reset();
  (void)ra8_mstp_init();
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

static void test_init_null_cfg(void)
{
  TEST_BEGIN("sram init null cfg");
  prep();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sram_init(nullptr));
  TEST_END("sram init null cfg");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_happy_disabled(void)
{
  TEST_BEGIN("sram init happy (all banks disabled)");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* Each SRAMCRn should be 0 (ECC off, OAD=int, latch off). */
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra8_sram_bank_count; ++bank) {
    TEST_ASSERT_EQ(0, *ra8_sram_cr_ptr(regs, bank));
    TEST_ASSERT_EQ(0, *ra8_sram_eccrgn_ptr(regs, bank));
  }
  /* Wait state cleared (cfg requested false). */
  TEST_ASSERT_EQ(0, regs->SRAMWTSC);
  TEST_END("sram init happy (all banks disabled)");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_with_ecc_check_bank0(void)
{
  TEST_BEGIN("sram init with ECC checking on bank 0");
  prep();
  ra8_sram_config_t cfg                                   = make_default_cfg();
  cfg.banks[k_ra8_sram_test_bank_first].ecc_mode          = k_ra8_sram_ecc_with_chk;
  cfg.banks[k_ra8_sram_test_bank_first].on_error          = k_ra8_sram_on_error_interrupt;
  cfg.banks[k_ra8_sram_test_bank_first].enable_1bit_latch = true;
  cfg.banks[k_ra8_sram_test_bank_first].eccrgn            = k_ra8_sram_region_512kb;
  cfg.banks[k_ra8_sram_test_bank_first].zero_init         = true;

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  volatile r_sram_regs_t* regs = ra8_sram_regs();
  const uint8_t           cr   = *ra8_sram_cr_ptr(regs, (uint8_t)k_ra8_sram_test_bank_first);
  const uint8_t expect = (uint8_t)k_ra8_sram_eccmod_with_chk | (uint8_t)k_ra8_sram_cr_mask_e1stsen;
  TEST_ASSERT_EQ(expect, cr);
  TEST_ASSERT_EQ(k_ra8_sram_eccrgn_512kb,
                 *ra8_sram_eccrgn_ptr(regs, (uint8_t)k_ra8_sram_test_bank_first));
  /* Other banks should still be 0. */
  TEST_ASSERT_EQ(0, *ra8_sram_cr_ptr(regs, (uint8_t)k_ra8_sram_test_bank_mid));

  /* PRCR should be re-locked (low bit cleared). */
  TEST_ASSERT_EQ(k_ra8_sram_prcr_lock, regs->SRAMPRCR_S);
  TEST_END("sram init with ECC checking on bank 0");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_invalid_mode(void)
{
  TEST_BEGIN("sram init invalid ecc_mode");
  prep();
  ra8_sram_config_t cfg                          = make_default_cfg();
  cfg.banks[k_ra8_sram_test_bank_first].ecc_mode = (ra8_sram_ecc_mode_t)k_t_ecc_mode_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_init(&cfg));
  TEST_END("sram init invalid ecc_mode");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_invalid_eccrgn_for_bank3(void)
{
  TEST_BEGIN("sram init bank3 cannot exceed 128 KB region");
  prep();
  ra8_sram_config_t cfg                       = make_default_cfg();
  cfg.banks[k_ra8_sram_test_bank_last].eccrgn = k_ra8_sram_region_256kb;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_init(&cfg));
  TEST_END("sram init bank3 cannot exceed 128 KB region");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_preserves_wait_state(void)
{
  TEST_BEGIN("sram init preserves the wait state cgc_init set");
  prep();
  volatile r_sram_regs_t* regs = ra8_sram_regs();
  /* Stand in for what ra8_cgc_init leaves behind (HUM Ch 58.3.7 p 3540). */
  regs->SRAMWTSC = (uint8_t)k_ra8_sram_wtsc_wten;

  ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  /* Configuring ECC says nothing about ICLK, so it must not disturb the
   * wait state. A zero-initialised config used to clear it here, which
   * is how two demos in this tree silently took their own memory system
   * outside guaranteed operation (tracker #524). */
  TEST_ASSERT_EQ(k_ra8_sram_wtsc_wten, regs->SRAMWTSC);
  TEST_END("sram init preserves the wait state cgc_init set");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_init_with_security(void)
{
  TEST_BEGIN("sram init applies SRAMSAR / SRAMESAR / SRAMSABARn");
  prep();
  ra8_sram_config_t cfg                                    = make_default_cfg();
  cfg.apply_security                                       = true;
  cfg.security.bank_ns[k_ra8_sram_test_bank_one]           = true;
  cfg.security.wtsc_ns                                     = true;
  cfg.security.ecc_region_ns                               = true;
  cfg.security.boundary_offset[k_ra8_sram_test_bank_first] = 0U;
  cfg.security.boundary_offset[k_ra8_sram_test_bank_one]   = (uint32_t)k_ra8_sram_test_sabar_off;
  cfg.security.boundary_offset[k_ra8_sram_test_bank_mid]   = 0U;
  cfg.security.boundary_offset[k_ra8_sram_test_bank_last]  = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));

  volatile r_sram_cpscu_regs_t* cpscu = ra8_sram_cpscu_regs();
  const uint32_t                expected_sar =
    (uint32_t)k_ra8_sram_sar_bit_sa1 | (uint32_t)k_ra8_sram_sar_bit_wtsa;
  TEST_ASSERT_EQ(expected_sar, cpscu->SRAMSAR);
  TEST_ASSERT_EQ(k_ra8_sram_esar_bit_esa, cpscu->SRAMESAR);
  TEST_ASSERT_EQ(k_ra8_sram_test_sabar_off, cpscu->SRAMSABAR[k_ra8_sram_test_bank_one]);
  TEST_END("sram init applies SRAMSAR / SRAMESAR / SRAMSABARn");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_deinit_clears_cr(void)
{
  TEST_BEGIN("sram deinit clears SRAMCRn");
  prep();
  ra8_sram_config_t cfg                          = make_default_cfg();
  cfg.banks[k_ra8_sram_test_bank_first].ecc_mode = k_ra8_sram_ecc_no_check;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_deinit());

  volatile r_sram_regs_t* regs = ra8_sram_regs();
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra8_sram_bank_count; ++bank) {
    TEST_ASSERT_EQ(0, *ra8_sram_cr_ptr(regs, bank));
    TEST_ASSERT_EQ(0, *ra8_sram_eccrgn_ptr(regs, bank));
  }
  TEST_ASSERT_EQ(0, regs->SRAMWTSC);
  TEST_END("sram deinit clears SRAMCRn");
}

/**
 * @par MC/DC:
 * (no compound decisions in this test -- exercises the public-API
 * happy path / error-rejection contract; no `&&` or `||` in the
 * code under test that this case touches)
 */
static void test_enter_exit_stop(void)
{
  TEST_BEGIN("sram enter_stop / exit_stop ok");
  prep();
  const ra8_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_init(&cfg));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_enter_stop((uint8_t)k_ra8_sram_test_bank_mid));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sram_exit_stop((uint8_t)k_ra8_sram_test_bank_mid));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_enter_stop((uint8_t)k_ra8_sram_test_bank_bad));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_sram_exit_stop((uint8_t)k_ra8_sram_test_bank_bad));
  TEST_END("sram enter_stop / exit_stop ok");
}

int main(void)
{
  test_init_null_cfg();
  test_init_happy_disabled();
  test_init_with_ecc_check_bank0();
  test_init_invalid_mode();
  test_init_invalid_eccrgn_for_bank3();
  test_init_preserves_wait_state();
  test_init_with_security();
  test_deinit_clears_cr();
  test_enter_exit_stop();
  return 0;
}
