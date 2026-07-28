/**
 * @file test_ra8_sdramc.c
 * @brief Unit tests for the SDRAMC driver (ra8_sdramc.c)
 *
 * @details
 * Exercises the SDRAM bring-up sequence for the EK-RA8D2 ISSI
 * IS42S32160F (512 Mbit, 16M x 32). Asserts the final SDRAMC
 * register image against the FSP `R_BSP_SdramInit` reference, plus
 * the deinit / refresh / status / power-transition lifecycle.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_pin_validator.h"
#include "ra8_sdramc.h"
#include "ra8_sdramc_regs.h"
#include "unity_minimal.h"

/**
 * @enum test_sdramc_u8_t
 * @brief Expected 8-bit SDRAMC register images after `ra8_sdramc_init`.
 */
typedef enum : uint8_t {
  k_test_sdccr_enabled = 0x11U, /**< BSIZE=01 (32-bit bus) | EXENB.   */
  k_test_sdamod_be     = 0x01U, /**< SDAMOD.BE: continuous access on. */
  k_test_sdadr_mxc     = 0x01U, /**< SDADR.MXC=01b: 9-bit column.     */
} test_sdramc_u8_t;

/**
 * @enum test_sdramc_u16_t
 * @brief Expected 16-bit SDRAMC register images after `ra8_sdramc_init`.
 */
typedef enum : uint16_t {
  k_test_sdir   = 0x0088U, /**< ARFI=11, ARFC=8, PRC=3.                */
  k_test_sdmod  = 0x0230U, /**< LMR: single-loc, CL=3, seq, BL=1.      */
  k_test_sdrfcr = 0xB383U, /**< REFW=12 cyc, refresh/900 cyc (7.2 us). */
} test_sdramc_u16_t;

/**
 * @enum test_sdramc_u32_t
 * @brief Expected 32-bit SDRAMC register image after `ra8_sdramc_init`.
 */
typedef enum : uint32_t {
  k_test_sdtr = 0x00053703UL, /**< RAS=6,RCD=4,RP=4,WR=2,CL=3 cycles. */
} test_sdramc_u32_t;

/** @brief Reset the mock MMIO backing and the pin-validator bitmap. */
static void reset_world(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
}

/**
 * @brief `ra8_sdramc_init` programs the IS42S32160F register image.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test -- `ra8_sdramc_init`
 * and its helpers use only single-condition `if` checks, so this
 * exercises the happy-path register-programming contract)
 */
static void test_init_programs_registers(void)
{
  TEST_BEGIN("ra8_sdramc_init programs the IS42S32160F register image");
  reset_world();

  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());

  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  TEST_ASSERT_EQ(k_test_sdccr_enabled, reg->SDCCR);
  TEST_ASSERT_EQ(0, reg->SDCMOD);
  TEST_ASSERT_EQ(k_test_sdamod_be, reg->SDAMOD);
  TEST_ASSERT_EQ(k_test_sdir, reg->SDIR);
  TEST_ASSERT_EQ(k_test_sdmod, reg->SDMOD);
  TEST_ASSERT_EQ(k_test_sdtr, reg->SDTR);
  TEST_ASSERT_EQ(k_test_sdadr_mxc, reg->SDADR);
  TEST_ASSERT_EQ(k_test_sdrfcr, reg->SDRFCR);
  TEST_ASSERT_EQ(1, reg->SDRFEN);
  TEST_ASSERT_EQ(1, reg->SDICR);
  TEST_ASSERT_EQ(1, *ra8_sdramc_sdckocr());

  TEST_END("ra8_sdramc_init programs the IS42S32160F register image");
}

/* ---- lifecycle + power ---- */

/**
 * @brief `ra8_sdramc_deinit` disables refresh and clears SDCCR.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test)
 */
static void test_deinit(void)
{
  TEST_BEGIN("sdramc deinit");
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_deinit());
  volatile r_sdramc_regs_t* reg = ra8_sdramc();
  TEST_ASSERT_EQ(0, reg->SDRFEN);
  TEST_ASSERT_EQ(0, reg->SDCCR);
  TEST_END("sdramc deinit");
}

/**
 * @brief `ra8_sdramc_set_refresh_interval` writes SDRFCR verbatim.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test)
 */
static void test_set_refresh_interval(void)
{
  TEST_BEGIN("sdramc set_refresh_interval");
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_set_refresh_interval(0x00ABU));
  TEST_ASSERT_EQ(0x00ABU, ra8_sdramc()->SDRFCR);
  TEST_END("sdramc set_refresh_interval");
}

/**
 * @brief `ra8_sdramc_get_status` reports the refresh-enable bit.
 *
 * @par MC/DC:
 * Decision: ``out_enabled == nullptr`` (one atomic condition).
 * - Vector 1: out_enabled=&enabled -> k_ra8_ok (varies condition false)
 * - Vector 2: out_enabled=nullptr  -> k_ra8_err_null_ptr (true)
 * N+1 = 2 vectors for N=1: minimal MC/DC for the NULL guard.
 */
static void test_get_status(void)
{
  TEST_BEGIN("sdramc get_status");
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());

  uint8_t enabled = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_get_status(&enabled));
  TEST_ASSERT_EQ(1, enabled);
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, ra8_sdramc_get_status(nullptr));
  TEST_END("sdramc get_status");
}

/**
 * @brief `enter_stop` / `exit_stop` toggle the auto-refresh enable.
 *
 * @par MC/DC:
 * (no compound decisions in the code under test)
 */
static void test_power_transition(void)
{
  TEST_BEGIN("sdramc power transition");
  reset_world();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_init());
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_enter_stop());
  TEST_ASSERT_EQ(0, ra8_sdramc()->SDRFEN);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_sdramc_exit_stop());
  TEST_ASSERT_EQ(1, ra8_sdramc()->SDRFEN);
  TEST_END("sdramc power transition");
}

int32_t main(void)
{
  test_init_programs_registers();
  test_deinit();
  test_set_refresh_interval();
  test_get_status();
  test_power_transition();
  (void)fprintf(stderr, "[OK ] test_ra8_sdramc.c\n");
  return 0;
}
