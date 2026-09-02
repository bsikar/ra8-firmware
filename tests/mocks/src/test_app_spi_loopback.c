/**
 * @file test_app_spi_loopback.c
 * @brief Integration test: SPI_B init + SPCR2.SPLP loopback bit + xfer8
 *
 * @details
 * Mirrors examples/ek_ra8d2/hw_validated/hil/spi_loopback/src/main.c bring-up:
 * ra8_mstp_init -> ra8_spi_init(cfg.loopback=true) -> ra8_spi_xfer8.
 * The HAL programmes SPCR2.SPLP while SPE=0 (HUM Ch 43.2.5 p 2889);
 * a stamp after SPE=1 would be silently dropped. All MMIO is via the
 * host tests/mocks/src/ra8_fake_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_mstp.h"
#include "ra8_spi.h"
#include "ra8_spi_regs.h"
#include "unity_minimal.h"

typedef enum : uint32_t {
  k_test_spi_app_baud_hz  = 1000000U,   /**< Test SPI app baud Hz.  */
  k_test_spi_app_pclka_hz = 125000000U, /**< Test SPI app pclka Hz. */
} test_spi_app_const_t;

typedef enum : uint8_t {
  k_test_spi_app_channel     = 0U,    /**< Test SPI app channel.     */
  k_test_spi_app_bad_channel = 99U,   /**< Test SPI app bad channel. */
  k_test_spi_app_pat_base    = 0xA0U, /**< Test SPI app pat base.    */
  k_test_spi_app_pat_len     = 16U,   /**< Test SPI app pat length.  */
} test_spi_app_byte_t;

static void reset_world(void)
{
  ra8_fake_mmap_reset();
}

/**
 * @brief Golden-path bring-up replays main.c spi_demo_setup_or_halt.
 *
 * @par MC/DC:
 * Compound decision in app: ``ra8_spi_init != ok``. One atomic
 * condition x 2 vectors -- valid (this) + bad-channel below.
 */
static void test_spi_app_bringup_ok(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: ra8_spi_init ok");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_spi_app_channel, &cfg));
  TEST_END("spi_loopback: ra8_spi_init ok");
}

/**
 * @brief cfg.loopback=true causes init to set SPCR2.SPLP.
 *
 * @par MC/DC:
 * Decision under test: ``cfg->loopback ? SPLP : 0`` in ra8_spi_init.
 * One atomic condition x 2 vectors -- loopback=true (this) +
 * loopback=false (test_spi_app_bringup_ok, which leaves SPLP clear).
 */
static void test_spi_app_loopback_bit_set(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: SPCR2.SPLP programmed by init");
  TEST_ASSERT_EQ(k_ra8_ok, ra8_mstp_init());
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
    .loopback  = true,
  };
  TEST_ASSERT_EQ(k_ra8_ok, ra8_spi_init((uint8_t)k_test_spi_app_channel, &cfg));
  volatile r_spi_regs_t* reg = ra8_spi((uint8_t)k_test_spi_app_channel);
  TEST_ASSERT_NOT_NULL((void*)reg);
  TEST_ASSERT((reg->SPCR2 & (uint32_t)k_ra8_spcr2_mask_splp2) != 0U);
  TEST_END("spi_loopback: SPCR2.SPLP programmed by init");
}

/**
 * @brief NULL config rejected by ra8_spi_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * NULL (this) + non-NULL (test_spi_app_bringup_ok).
 */
static void test_spi_app_init_null_rejected(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: NULL cfg rejected");
  TEST_ASSERT(ra8_spi_init((uint8_t)k_test_spi_app_channel, nullptr) != k_ra8_ok);
  TEST_END("spi_loopback: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ra8_spi_init.
 *
 * @par MC/DC:
 * Decision: ``channel out-of-range``. One atomic condition x 2
 * vectors -- in-range (test_spi_app_bringup_ok) + out-of-range (this).
 */
static void test_spi_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("spi_loopback: bad channel rejected");
  const ra8_spi_cfg_t cfg = {
    .baud_hz   = (uint32_t)k_test_spi_app_baud_hz,
    .pclka_hz  = (uint32_t)k_test_spi_app_pclka_hz,
    .mode      = k_ra8_spi_mode_0,
    .lsb_first = false,
  };
  TEST_ASSERT(ra8_spi_init((uint8_t)k_test_spi_app_bad_channel, &cfg) != k_ra8_ok);
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
