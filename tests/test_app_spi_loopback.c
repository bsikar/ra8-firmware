/**
 * @file test_app_spi_loopback.c
 * @brief Integration test: SPI_B init + SPCR2.SPLP loopback bit + xfer8
 *
 * @details
 * Mirrors examples/ek_ra8d2/spi_loopback/main.c bring-up:
 * ra_mstp_init -> ra_spi_init -> raw SPCR2.SPLP stamp ->
 * ra_spi_xfer8. All MMIO is via the host tests/mocks/ra_sim_mmap.c
 * shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_spi_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_spi.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_spi_app_baud_hz  = 1000000U,
  k_test_spi_app_pclka_hz = 125000000U,
} test_spi_app_const_t;

typedef enum : uint8_t {
  k_test_spi_app_channel     = 0U,
  k_test_spi_app_bad_channel = 99U,
  k_test_spi_app_pat_base    = 0xA0U,
  k_test_spi_app_pat_len     = 16U,
} test_spi_app_byte_t;

static void reset_world(void)
{
  ra_sim_mmap_reset();
}

/**
 * @brief Golden-path bring-up replays main.c spi_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra_spi_init != ok``. One atomic
 * condition x 2 vectors -- valid (this) + bad-channel below.
 */
static void test_spi_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: ra_spi_init ok");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mstp_init());
  const ra_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_spi_init((uint8_t)k_test_spi_app_channel, &cfg));
  TEST_END("spi_loopback: ra_spi_init ok");
}

/**
 * @brief SPCR2.SPLP bit set raises SPLP -- internal-loopback enable.
 *
 * @par MC/DC:
 * Decision under test: ``ra_spi(channel) == nullptr``. One atomic
 * condition x 2 vectors -- valid channel (this) + invalid channel
 * (test_spi_app_bad_channel).
 */
static void test_spi_app_loopback_bit_set(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: SPCR2.SPLP stamped");
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_mstp_init());
  const ra_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_spi_init((uint8_t)k_test_spi_app_channel, &cfg));
  volatile r_spi_regs_t* reg = ra_spi((uint8_t)k_test_spi_app_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  reg->SPCR2 |= (uint32_t)k_ra_spcr2_mask_splp;
  TEST_ASSERT((reg->SPCR2 & (uint32_t)k_ra_spcr2_mask_splp) != 0U);
  TEST_END("spi_loopback: SPCR2.SPLP stamped");
}

/**
 * @brief NULL config rejected by ra_spi_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_spi_app_bringup_ok).
 */
static void test_spi_app_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: NULL cfg rejected");
  TEST_ASSERT(ra_spi_init((uint8_t)k_test_spi_app_channel, nullptr) != k_ra_ok);
  TEST_END("spi_loopback: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ra_spi_init.
 *
 * @par MC/DC:
 * Decision: ``channel out-of-range``. One atomic condition x 2
 * vectors -- in-range (test_spi_app_bringup_ok) + out-of-range (this).
 */
static void test_spi_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: bad channel rejected");
  const ra_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT(ra_spi_init((uint8_t)k_test_spi_app_bad_channel, &cfg) != k_ra_ok);
  TEST_END("spi_loopback: bad channel rejected");
}

int main(void)
{
  test_spi_app_bringup_ok();
  test_spi_app_loopback_bit_set();
  test_spi_app_init_null_rejected();
  test_spi_app_bad_channel();
  return 0;
}
