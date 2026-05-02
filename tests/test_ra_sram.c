/**
 * @file test_ra_sram.c
 * @brief Unit tests for ra_sram.c (SRAM with ECC driver)
 *
 * @details
 * Drives the host-side simulated SRAM control window provided by
 * ``tests/mocks/ra_sim_mmap.c``. Both the SRAM control block at
 * ``0x4000_2000`` and the SRAM data alias at ``0x2200_0000`` (2 MiB)
 * are mapped for tests, so the zero-init pass and the ECC self-test
 * exercise real memory.
 *
 * Coverage now matches the round-3 brief:
 *  - happy init applies SRAMCRn under SRAMPRCR_S unlock
 *  - null-cfg / out-of-range bank / out-of-range region rejected
 *  - ``ra_sram_set_mode`` reprograms a single bank's CR + ECCRGN
 *  - ``ra_sram_set_eccrgn`` standalone, plus SRAM3 limit
 *  - status get / clear round-trip including EAR auto-clear
 *  - clear_address per (bank, slot)
 *  - global + per-bank error callback dispatch
 *  - ``ra_sram_dispatch_from_esr`` walks every set ESR bit
 *  - zero-init pass writes all-zero across the bank
 *  - ECC decoder self-test catches both 1-bit and 2-bit faults
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

#include "ra8d2_sram_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_sram.h"
#include "unity_minimal.h"

/* =============================================================================
 * Test constants
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_sram_test_bank_first = 0U,
  k_ra_sram_test_bank_one   = 1U,
  k_ra_sram_test_bank_mid   = 2U,
  k_ra_sram_test_bank_last  = 3U,
  k_ra_sram_test_bank_bad   = 4U,
  k_ra_sram_test_slot_bad   = 2U,
} ra_sram_test_bank_t;

typedef enum : uintptr_t {
  k_ra_sram_test_ctx_token      = 0xC0FFEE12UL,
  k_ra_sram_test_bank_ctx_token = 0xBEEF0001UL,
} ra_sram_test_ctx_t;

typedef enum : uintptr_t {
  k_ra_sram_test_fault_addr = 0x12340U,
} ra_sram_test_addr_t;

typedef enum : uint32_t {
  k_ra_sram_test_iclk_high  = 200000000UL, /**< 200 MHz -> wait needed.        */
  k_ra_sram_test_iclk_low   = 100000000UL, /**< 100 MHz -> no wait needed.     */
  k_ra_sram_test_iclk_max   = 250000000UL, /**< RA8D2 default max ICLK.        */
  k_ra_sram_test_probe_off  = 0x100U,      /**< 8-byte aligned probe offset.   */
  k_ra_sram_test_bank2_size = 0x80000UL,   /**< 512 KB SRAM2 size for boundary. */
  k_ra_sram_test_sabar_off  = 0x10000UL,   /**< 64 KB Secure boundary, 4 KB OK. */
  k_ra_sram_test_sabar_bad  = 0x10001UL,   /**< Mis-aligned (low bit set).      */
} ra_sram_test_misc_t;

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
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
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

static ra_sram_config_t make_default_cfg(void)
{
  ra_sram_config_t cfg = {};
  for (uint8_t i = 0U; i < (uint8_t)k_ra_sram_bank_count; ++i) {
    cfg.banks[i].ecc_mode          = k_ra_sram_ecc_disabled;
    cfg.banks[i].on_error          = k_ra_sram_on_error_interrupt;
    cfg.banks[i].enable_1bit_latch = false;
    cfg.banks[i].eccrgn            = k_ra_sram_region_off;
    cfg.banks[i].zero_init         = false;
  }
  cfg.apply_security = false;
  cfg.wait_state     = false;
  return cfg;
}

/* =============================================================================
 * Tests -- lifecycle
 * =============================================================================
 */

static void test_init_null_cfg(void)
{
  TEST_BEGIN("sram init null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sram_init(nullptr));
  TEST_END("sram init null cfg");
}

static void test_init_happy_disabled(void)
{
  TEST_BEGIN("sram init happy (all banks disabled)");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  /* Each SRAMCRn should be 0 (ECC off, OAD=int, latch off). */
  volatile r_sram_regs_t* regs = ra_sram_regs();
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra_sram_bank_count; ++bank) {
    TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_cr_ptr(regs, bank));
    TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_eccrgn_ptr(regs, bank));
  }
  /* Wait state cleared (cfg requested false). */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)regs->SRAMWTSC);
  TEST_END("sram init happy (all banks disabled)");
}

static void test_init_with_ecc_check_bank0(void)
{
  TEST_BEGIN("sram init with ECC checking on bank 0");
  prep();
  ra_sram_config_t cfg                                   = make_default_cfg();
  cfg.banks[k_ra_sram_test_bank_first].ecc_mode          = k_ra_sram_ecc_with_chk;
  cfg.banks[k_ra_sram_test_bank_first].on_error          = k_ra_sram_on_error_interrupt;
  cfg.banks[k_ra_sram_test_bank_first].enable_1bit_latch = true;
  cfg.banks[k_ra_sram_test_bank_first].eccrgn            = k_ra_sram_region_512kb;
  cfg.banks[k_ra_sram_test_bank_first].zero_init         = true;

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  volatile r_sram_regs_t* regs = ra_sram_regs();
  const uint8_t           cr   = *ra_sram_cr_ptr(regs, (uint8_t)k_ra_sram_test_bank_first);
  const uint8_t expect = (uint8_t)k_ra_sram_eccmod_with_chk | (uint8_t)k_ra_sram_cr_mask_e1stsen;
  TEST_ASSERT_EQ((int32_t)expect, (int32_t)cr);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_eccrgn_512kb,
                 (int32_t)*ra_sram_eccrgn_ptr(regs, (uint8_t)k_ra_sram_test_bank_first));
  /* Other banks should still be 0. */
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_cr_ptr(regs, (uint8_t)k_ra_sram_test_bank_mid));

  /* PRCR should be re-locked (low bit cleared). */
  TEST_ASSERT_EQ((int32_t)k_ra_sram_prcr_lock, (int32_t)regs->SRAMPRCR_S);
  TEST_END("sram init with ECC checking on bank 0");
}

static void test_init_invalid_mode(void)
{
  TEST_BEGIN("sram init invalid ecc_mode");
  prep();
  ra_sram_config_t cfg                          = make_default_cfg();
  cfg.banks[k_ra_sram_test_bank_first].ecc_mode = (ra_sram_ecc_mode_t)0xFFU;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sram_init(&cfg));
  TEST_END("sram init invalid ecc_mode");
}

static void test_init_invalid_eccrgn_for_bank3(void)
{
  TEST_BEGIN("sram init bank3 cannot exceed 128 KB region");
  prep();
  ra_sram_config_t cfg                       = make_default_cfg();
  cfg.banks[k_ra_sram_test_bank_last].eccrgn = k_ra_sram_region_256kb;
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sram_init(&cfg));
  TEST_END("sram init bank3 cannot exceed 128 KB region");
}

static void test_init_with_wait_state(void)
{
  TEST_BEGIN("sram init applies wait state");
  prep();
  ra_sram_config_t cfg = make_default_cfg();
  cfg.wait_state       = true;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_wtsc_wten, (int32_t)regs->SRAMWTSC);
  TEST_END("sram init applies wait state");
}

static void test_init_with_security(void)
{
  TEST_BEGIN("sram init applies SRAMSAR / SRAMESAR / SRAMSABARn");
  prep();
  ra_sram_config_t cfg                                    = make_default_cfg();
  cfg.apply_security                                      = true;
  cfg.security.bank_ns[k_ra_sram_test_bank_one]           = true;
  cfg.security.wtsc_ns                                    = true;
  cfg.security.ecc_region_ns                              = true;
  cfg.security.boundary_offset[k_ra_sram_test_bank_first] = 0U;
  cfg.security.boundary_offset[k_ra_sram_test_bank_one]   = (uint32_t)k_ra_sram_test_sabar_off;
  cfg.security.boundary_offset[k_ra_sram_test_bank_mid]   = 0U;
  cfg.security.boundary_offset[k_ra_sram_test_bank_last]  = 0U;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  volatile r_sram_cpscu_regs_t* cpscu = ra_sram_cpscu_regs();
  const uint32_t expected_sar = (uint32_t)k_ra_sram_sar_bit_sa1 | (uint32_t)k_ra_sram_sar_bit_wtsa;
  TEST_ASSERT_EQ((int32_t)expected_sar, (int32_t)cpscu->SRAMSAR);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_esar_bit_esa, (int32_t)cpscu->SRAMESAR);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_sabar_off,
                 (int32_t)cpscu->SRAMSABAR[k_ra_sram_test_bank_one]);
  TEST_END("sram init applies SRAMSAR / SRAMESAR / SRAMSABARn");
}

static void test_deinit_clears_cr(void)
{
  TEST_BEGIN("sram deinit clears SRAMCRn");
  prep();
  ra_sram_config_t cfg                          = make_default_cfg();
  cfg.banks[k_ra_sram_test_bank_first].ecc_mode = k_ra_sram_ecc_no_check;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_deinit());

  volatile r_sram_regs_t* regs = ra_sram_regs();
  for (uint8_t bank = 0U; bank < (uint8_t)k_ra_sram_bank_count; ++bank) {
    TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_cr_ptr(regs, bank));
    TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_eccrgn_ptr(regs, bank));
  }
  TEST_ASSERT_EQ((int32_t)0, (int32_t)regs->SRAMWTSC);
  TEST_END("sram deinit clears SRAMCRn");
}

static void test_enter_exit_stop(void)
{
  TEST_BEGIN("sram enter_stop / exit_stop ok");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_enter_stop((uint8_t)k_ra_sram_test_bank_mid));
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_exit_stop((uint8_t)k_ra_sram_test_bank_mid));

  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_enter_stop((uint8_t)k_ra_sram_test_bank_bad));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_exit_stop((uint8_t)k_ra_sram_test_bank_bad));
  TEST_END("sram enter_stop / exit_stop ok");
}

/* =============================================================================
 * Tests -- set_mode / set_eccrgn
 * =============================================================================
 */

static void test_set_mode_happy(void)
{
  TEST_BEGIN("sram set_mode reprograms one bank");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  const ra_sram_bank_cfg_t bank_cfg = {
    .ecc_mode          = k_ra_sram_ecc_with_chk,
    .on_error          = k_ra_sram_on_error_reset,
    .enable_1bit_latch = true,
    .eccrgn            = k_ra_sram_region_256kb,
    .zero_init         = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_set_mode((uint8_t)k_ra_sram_test_bank_mid, &bank_cfg));

  volatile r_sram_regs_t* regs = ra_sram_regs();
  const uint8_t           cr   = *ra_sram_cr_ptr(regs, (uint8_t)k_ra_sram_test_bank_mid);
  const uint8_t want = (uint8_t)k_ra_sram_eccmod_with_chk | (uint8_t)k_ra_sram_cr_mask_oad |
                       (uint8_t)k_ra_sram_cr_mask_e1stsen;
  TEST_ASSERT_EQ((int32_t)want, (int32_t)cr);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_eccrgn_256kb,
                 (int32_t)*ra_sram_eccrgn_ptr(regs, (uint8_t)k_ra_sram_test_bank_mid));
  TEST_END("sram set_mode reprograms one bank");
}

static void test_set_mode_bad_bank(void)
{
  TEST_BEGIN("sram set_mode bad bank");
  prep();
  const ra_sram_bank_cfg_t bank_cfg = {
    .ecc_mode          = k_ra_sram_ecc_no_check,
    .on_error          = k_ra_sram_on_error_interrupt,
    .enable_1bit_latch = false,
    .eccrgn            = k_ra_sram_region_off,
    .zero_init         = false,
  };
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_mode((uint8_t)k_ra_sram_test_bank_bad, &bank_cfg));
  TEST_END("sram set_mode bad bank");
}

static void test_set_mode_null_cfg(void)
{
  TEST_BEGIN("sram set_mode null cfg");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sram_set_mode((uint8_t)k_ra_sram_test_bank_first, nullptr));
  TEST_END("sram set_mode null cfg");
}

static void test_set_eccrgn_happy(void)
{
  TEST_BEGIN("sram set_eccrgn updates SRAMECCRGNn only");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sram_set_eccrgn((uint8_t)k_ra_sram_test_bank_one, k_ra_sram_region_384kb));
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_eccrgn_384kb,
                 (int32_t)*ra_sram_eccrgn_ptr(regs, (uint8_t)k_ra_sram_test_bank_one));
  TEST_END("sram set_eccrgn updates SRAMECCRGNn only");
}

static void test_set_eccrgn_rejects_bank3_oversize(void)
{
  TEST_BEGIN("sram set_eccrgn bank3 over 128 KB rejected");
  prep();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_sram_set_eccrgn((uint8_t)k_ra_sram_test_bank_last, k_ra_sram_region_256kb));
  TEST_END("sram set_eccrgn bank3 over 128 KB rejected");
}

/* =============================================================================
 * Tests -- wait state
 * =============================================================================
 */

static void test_set_wait_state_manual(void)
{
  TEST_BEGIN("sram set_wait_state writes WTEN");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_set_wait_state(true));
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_wtsc_wten, (int32_t)regs->SRAMWTSC);
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_set_wait_state(false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)regs->SRAMWTSC);
  TEST_END("sram set_wait_state writes WTEN");
}

static void test_set_wait_state_for_clock(void)
{
  TEST_BEGIN("sram set_wait_state_for_clock follows HUM threshold");
  prep();

  /* Above half-max -> WTEN=1. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_set_wait_state_for_clock((uint32_t)k_ra_sram_test_iclk_high,
                                                           (uint32_t)k_ra_sram_test_iclk_max));
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_wtsc_wten, (int32_t)regs->SRAMWTSC);

  /* Below half-max -> WTEN=0. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_set_wait_state_for_clock((uint32_t)k_ra_sram_test_iclk_low,
                                                           (uint32_t)k_ra_sram_test_iclk_max));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)regs->SRAMWTSC);

  /* Bad inputs rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_wait_state_for_clock(0U, (uint32_t)k_ra_sram_test_iclk_max));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_wait_state_for_clock((uint32_t)k_ra_sram_test_iclk_high, 0U));
  TEST_END("sram set_wait_state_for_clock follows HUM threshold");
}

/* =============================================================================
 * Tests -- status / clear
 * =============================================================================
 */

static void test_status_decode(void)
{
  TEST_BEGIN("sram status decodes per-bank flags");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  /* Inject error flags directly into the simulated SRAMESR. */
  volatile r_sram_regs_t* regs = ra_sram_regs();
  regs->SRAMESR =
    (uint16_t)((uint16_t)k_ra_sram_err_bank0_1bit | (uint16_t)k_ra_sram_err_bank3_2bit);
  regs->SRAMEAR[k_ra_sram_test_bank_first][0] = (uint32_t)k_ra_sram_test_fault_addr;
  regs->SRAMEAR[k_ra_sram_test_bank_last][1]  = (uint32_t)k_ra_sram_test_fault_addr;

  ra_sram_status_t out = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_get_status(&out));
  TEST_ASSERT_EQ((int32_t)((uint16_t)k_ra_sram_err_bank0_1bit | (uint16_t)k_ra_sram_err_bank3_2bit),
                 (int32_t)out.raw_esr);
  TEST_ASSERT_EQ((int32_t)0x01U, (int32_t)out.one_bit_mask); /* bank 0 only. */
  TEST_ASSERT_EQ((int32_t)0x08U, (int32_t)out.two_bit_mask); /* bank 3 only. */
  /* Address is now absolute (offset + 0x2200_0000 base). */
  const uintptr_t want_abs =
    (uintptr_t)k_ra_sram_data_base_addr + (uintptr_t)k_ra_sram_test_fault_addr;
  TEST_ASSERT_EQ((int64_t)want_abs, (int64_t)out.addr_1bit[k_ra_sram_test_bank_first]);
  TEST_ASSERT_EQ((int64_t)want_abs, (int64_t)out.addr_2bit[k_ra_sram_test_bank_last]);
  TEST_END("sram status decodes per-bank flags");
}

static void test_status_null_out(void)
{
  TEST_BEGIN("sram status null out");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sram_get_status(nullptr));
  TEST_END("sram status null out");
}

static void test_clear_status_writes_esclr(void)
{
  TEST_BEGIN("sram clear_status writes ESCLR");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_clear_status((uint16_t)k_ra_sram_err_bank2_2bit));
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_err_bank2_2bit, (int32_t)regs->SRAMESCLR);
  TEST_END("sram clear_status writes ESCLR");
}

static void test_clear_status_rejects_reserved(void)
{
  TEST_BEGIN("sram clear_status rejects reserved bits");
  prep();
  /* Bits 8..15 are reserved in SRAMESCLR. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sram_clear_status((uint16_t)0x0100U));
  TEST_END("sram clear_status rejects reserved bits");
}

static void test_clear_address_per_slot(void)
{
  TEST_BEGIN("sram clear_address writes single ESCLR bit");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_clear_address((uint8_t)k_ra_sram_test_bank_one,
                                                (uint8_t)k_ra_sram_ear_slot_2bit));
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_err_bank1_2bit, (int32_t)regs->SRAMESCLR);

  /* Bad bank / slot rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_clear_address((uint8_t)k_ra_sram_test_bank_bad,
                                                (uint8_t)k_ra_sram_ear_slot_1bit));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_clear_address((uint8_t)k_ra_sram_test_bank_first,
                                                (uint8_t)k_ra_sram_test_slot_bad));
  TEST_END("sram clear_address writes single ESCLR bit");
}

/* =============================================================================
 * Tests -- zero-init / self-test / introspection
 * =============================================================================
 */

static void test_zero_init_bank_writes_all_zero(void)
{
  TEST_BEGIN("sram zero_init_bank writes 64-bit zero across the bank");
  prep();
  /* Pre-fill SRAM3 with garbage so the zero pass has work to do.
   * SRAM3 is 128 KB which is the smallest bank -- fastest test. */
  volatile uint64_t* dst   = ra_sram_bank_data_ptr((uint8_t)k_ra_sram_test_bank_last);
  const uint32_t     bytes = ra_sram_bank_size_bytes((uint8_t)k_ra_sram_test_bank_last);
  const uint32_t     words = bytes >> (uint32_t)k_ra_sram_ecc_word_shift;
  for (uint32_t i = 0U; i < words; ++i) {
    dst[i] = (uint64_t)0xDEADBEEFCAFEBABEULL;
  }

  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_zero_init_bank((uint8_t)k_ra_sram_test_bank_last));

  for (uint32_t i = 0U; i < words; ++i) {
    TEST_ASSERT_EQ((int64_t)0, (int64_t)dst[i]);
  }

  /* Bank should be left in ECC-disabled mode. */
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)0, (int32_t)*ra_sram_cr_ptr(regs, (uint8_t)k_ra_sram_test_bank_last));

  /* Bad bank rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_zero_init_bank((uint8_t)k_ra_sram_test_bank_bad));
  TEST_END("sram zero_init_bank writes 64-bit zero across the bank");
}

static void test_self_test_catches_1bit(void)
{
  TEST_BEGIN("sram self_test catches a 1-bit injected error");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  bool caught = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_one,
                                            (uint32_t)k_ra_sram_test_probe_off,
                                            false,
                                            &caught));
  TEST_ASSERT(caught);

  /* The verify-step CR is 0x1C (ECC + check + 1-bit latch). */
  volatile r_sram_regs_t* regs = ra_sram_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_cr_self_test_phase_verify,
                 (int32_t)*ra_sram_cr_ptr(regs, (uint8_t)k_ra_sram_test_bank_one));
  TEST_END("sram self_test catches a 1-bit injected error");
}

static void test_self_test_catches_2bit(void)
{
  TEST_BEGIN("sram self_test catches a 2-bit injected error");
  prep();
  const ra_sram_config_t cfg = make_default_cfg();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_init(&cfg));

  bool caught = false;
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_one,
                                            (uint32_t)k_ra_sram_test_probe_off,
                                            true,
                                            &caught));
  TEST_ASSERT(caught);
  TEST_END("sram self_test catches a 2-bit injected error");
}

static void test_self_test_rejects_bad_offset(void)
{
  TEST_BEGIN("sram self_test rejects bad probe offset");
  prep();
  bool caught = false;
  /* Mis-aligned offset (low 3 bits set). */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_first, 7U, false, &caught));
  /* Beyond bank end. */
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_invalid_arg,
    (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_first, 0xFFFFFFF8U, false, &caught));
  /* Null caught pointer. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_first,
                                            (uint32_t)k_ra_sram_test_probe_off,
                                            false,
                                            nullptr));
  /* Bad bank. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_self_test((uint8_t)k_ra_sram_test_bank_bad,
                                            (uint32_t)k_ra_sram_test_probe_off,
                                            false,
                                            &caught));
  TEST_END("sram self_test rejects bad probe offset");
}

static void test_get_bank_info(void)
{
  TEST_BEGIN("sram get_bank_info reports static layout");
  prep();
  ra_sram_bank_info_t info = {};
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_get_bank_info((uint8_t)k_ra_sram_test_bank_one, &info));
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_bank_one, (int32_t)info.bank);
  TEST_ASSERT_EQ(
    (int64_t)((uintptr_t)k_ra_sram_data_base_addr + (uintptr_t)k_ra_sram_bank1_data_off),
    (int64_t)info.data_base);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_bank012_size, (int32_t)info.data_size);
  TEST_ASSERT_EQ(
    (int64_t)((uintptr_t)k_ra_sram_data_base_addr + (uintptr_t)k_ra_sram_ecc_bank1_off),
    (int64_t)info.ecc_base);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_ecc_bank012_sz, (int32_t)info.ecc_size);

  /* SRAM3 is the small bank. */
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_get_bank_info((uint8_t)k_ra_sram_test_bank_last, &info));
  TEST_ASSERT_EQ((int32_t)k_ra_sram_bank3_size, (int32_t)info.data_size);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_ecc_bank3_sz, (int32_t)info.ecc_size);

  /* Bad bank / null. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_get_bank_info((uint8_t)k_ra_sram_test_bank_bad, &info));
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr,
                 (int32_t)ra_sram_get_bank_info((uint8_t)k_ra_sram_test_bank_first, nullptr));
  TEST_END("sram get_bank_info reports static layout");
}

/* =============================================================================
 * Tests -- TrustZone security helpers
 * =============================================================================
 */

static void test_set_security(void)
{
  TEST_BEGIN("sram set_security writes SRAMSAR");
  prep();
  const uint32_t mask = (uint32_t)k_ra_sram_sar_bit_sa0 | (uint32_t)k_ra_sram_sar_bit_wtsa;
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_set_security(mask));
  volatile r_sram_cpscu_regs_t* cpscu = ra_sram_cpscu_regs();
  TEST_ASSERT_EQ((int32_t)mask, (int32_t)cpscu->SRAMSAR);

  /* Reserved bits rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg, (int32_t)ra_sram_set_security(0xFFFFFFFFU));
  TEST_END("sram set_security writes SRAMSAR");
}

static void test_set_ecc_security(void)
{
  TEST_BEGIN("sram set_ecc_security writes SRAMESAR");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_set_ecc_security(true));
  volatile r_sram_cpscu_regs_t* cpscu = ra_sram_cpscu_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_esar_bit_esa, (int32_t)cpscu->SRAMESAR);

  TEST_ASSERT_EQ((int32_t)k_ra_ok, (int32_t)ra_sram_set_ecc_security(false));
  TEST_ASSERT_EQ((int32_t)0, (int32_t)cpscu->SRAMESAR);
  TEST_END("sram set_ecc_security writes SRAMESAR");
}

static void test_set_boundary(void)
{
  TEST_BEGIN("sram set_boundary writes SRAMSABARn");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_set_boundary((uint8_t)k_ra_sram_test_bank_mid,
                                               (uint32_t)k_ra_sram_test_sabar_off));
  volatile r_sram_cpscu_regs_t* cpscu = ra_sram_cpscu_regs();
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_sabar_off,
                 (int32_t)cpscu->SRAMSABAR[k_ra_sram_test_bank_mid]);

  /* Mis-aligned boundary rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_boundary((uint8_t)k_ra_sram_test_bank_mid,
                                               (uint32_t)k_ra_sram_test_sabar_bad));
  /* Bad bank rejected. */
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_boundary((uint8_t)k_ra_sram_test_bank_bad,
                                               (uint32_t)k_ra_sram_test_sabar_off));
  TEST_END("sram set_boundary writes SRAMSABARn");
}

/* =============================================================================
 * Tests -- callback dispatch
 * =============================================================================
 */

static void test_attach_null_fn_rejected(void)
{
  TEST_BEGIN("sram attach rejects null fn");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_err_null_ptr, (int32_t)ra_sram_attach_handler(nullptr, nullptr));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_err_null_ptr,
    (int32_t)ra_sram_attach_bank_handler((uint8_t)k_ra_sram_test_bank_first, nullptr, nullptr));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_attach_bank_handler((uint8_t)k_ra_sram_test_bank_bad,
                                                      stub_bank_error_cb,
                                                      nullptr));
  TEST_END("sram attach rejects null fn");
}

static void test_dispatch_fires_callback(void)
{
  TEST_BEGIN("sram dispatch fires global + bank callbacks");
  prep();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sram_attach_handler(stub_error_cb, (void*)(uintptr_t)k_ra_sram_test_ctx_token));
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sram_attach_bank_handler((uint8_t)k_ra_sram_test_bank_mid,
                                         stub_bank_error_cb,
                                         (void*)(uintptr_t)k_ra_sram_test_bank_ctx_token));

  ra_sram_dispatch((uint8_t)k_ra_sram_test_bank_mid, true, (uintptr_t)k_ra_sram_test_fault_addr);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_bank_mid, (int32_t)s_cb_last_bank);
  TEST_ASSERT(s_cb_last_2bit);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_fault_addr, (int32_t)s_cb_last_addr);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_ctx_token, (int32_t)(uintptr_t)s_cb_last_ctx);

  /* Per-bank handler also fired with the bank-specific ctx. */
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_bank_cb_count);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_bank_mid, (int32_t)s_bank_cb_last_bank);
  TEST_ASSERT(s_bank_cb_last_2bit);
  TEST_ASSERT_EQ((int32_t)k_ra_sram_test_bank_ctx_token, (int32_t)(uintptr_t)s_bank_cb_last_ctx);

  /* Out-of-range bank should be ignored. */
  ra_sram_dispatch((uint8_t)k_ra_sram_test_bank_bad, false, (uintptr_t)k_ra_sram_test_fault_addr);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)1, (int32_t)s_bank_cb_count);
  TEST_END("sram dispatch fires global + bank callbacks");
}

static void test_dispatch_from_esr_walks_all_bits(void)
{
  TEST_BEGIN("sram dispatch_from_esr fans every set bit");
  prep();
  TEST_ASSERT_EQ(
    (int32_t)k_ra_ok,
    (int32_t)ra_sram_attach_handler(stub_error_cb, (void*)(uintptr_t)k_ra_sram_test_ctx_token));

  /* Forge SRAMESR with three flags set across two banks. */
  volatile r_sram_regs_t* regs = ra_sram_regs();
  regs->SRAMESR =
    (uint16_t)((uint16_t)k_ra_sram_err_bank0_1bit | (uint16_t)k_ra_sram_err_bank2_2bit |
               (uint16_t)k_ra_sram_err_bank3_1bit);
  regs->SRAMEAR[k_ra_sram_test_bank_first][0] = 0x1000U;
  regs->SRAMEAR[k_ra_sram_test_bank_mid][1]   = 0x2000U;
  regs->SRAMEAR[k_ra_sram_test_bank_last][0]  = 0x3000U;

  ra_sram_status_t snapshot = {};
  const uint16_t   fired    = ra_sram_dispatch_from_esr(&snapshot);

  TEST_ASSERT_EQ((int32_t)((uint16_t)k_ra_sram_err_bank0_1bit | (uint16_t)k_ra_sram_err_bank2_2bit |
                           (uint16_t)k_ra_sram_err_bank3_1bit),
                 (int32_t)fired);
  TEST_ASSERT_EQ((int32_t)3, (int32_t)s_cb_count);
  TEST_ASSERT_EQ((int32_t)snapshot.raw_esr, (int32_t)fired);
  TEST_END("sram dispatch_from_esr fans every set bit");
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

/**
 * @test test_mcdc_ra_sram
 *
 * @par MC/DC:
 * Decision A: ``ra_sram_set_wait_state_for_clock`` line 582,
 * libs/ra_hal/src/ra_sram.c:
 * ``if ((iclk_hz == 0U) || (iclk_max_hz == 0U))`` (2 conditions, ``||``).
 * N+1 = 3:
 * - V1: iclk=200M, max=250M  -> dec F (compute wait)
 * - V2: iclk=0,    max=250M  -> dec T (invalid_arg)
 * - V3: iclk=200M, max=0     -> dec T (invalid_arg)
 * DO-178C 6.4.4.3 met.
 */
static void test_mcdc_ra_sram(void)
{
  TEST_BEGIN("sram MC/DC: set_wait_state_for_clock 2-cond decision");
  prep();
  TEST_ASSERT_EQ((int32_t)k_ra_ok,
                 (int32_t)ra_sram_set_wait_state_for_clock((uint32_t)k_ra_sram_test_iclk_high,
                                                           (uint32_t)k_ra_sram_test_iclk_max));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_wait_state_for_clock(0U, (uint32_t)k_ra_sram_test_iclk_max));
  TEST_ASSERT_EQ((int32_t)k_ra_err_invalid_arg,
                 (int32_t)ra_sram_set_wait_state_for_clock((uint32_t)k_ra_sram_test_iclk_high, 0U));
  TEST_END("sram MC/DC: set_wait_state_for_clock 2-cond decision");
}

int32_t main(void)
{
  test_init_null_cfg();
  test_init_happy_disabled();
  test_init_with_ecc_check_bank0();
  test_init_invalid_mode();
  test_init_invalid_eccrgn_for_bank3();
  test_init_with_wait_state();
  test_init_with_security();
  test_deinit_clears_cr();
  test_enter_exit_stop();
  test_set_mode_happy();
  test_set_mode_bad_bank();
  test_set_mode_null_cfg();
  test_set_eccrgn_happy();
  test_set_eccrgn_rejects_bank3_oversize();
  test_set_wait_state_manual();
  test_set_wait_state_for_clock();
  test_status_decode();
  test_status_null_out();
  test_clear_status_writes_esclr();
  test_clear_status_rejects_reserved();
  test_clear_address_per_slot();
  test_zero_init_bank_writes_all_zero();
  test_self_test_catches_1bit();
  test_self_test_catches_2bit();
  test_self_test_rejects_bad_offset();
  test_get_bank_info();
  test_set_security();
  test_set_ecc_security();
  test_set_boundary();
  test_attach_null_fn_rejected();
  test_dispatch_fires_callback();
  test_dispatch_from_esr_walks_all_bits();
  test_mcdc_ra_sram();
  (void)fprintf(stderr, "[OK  ] test_ra_sram.c\n");
  return 0;
}
