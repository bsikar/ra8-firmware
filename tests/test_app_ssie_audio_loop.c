/**
 * @file test_app_ssie_audio_loop.c
 * @brief Integration test: SSIE I2S internal-loopback bring-up
 *
 * @details
 * Mirrors examples/ek_ra8d2/ssie_audio_loop/main.c bring-up flow:
 * ra_ssie_init -> ra_ssie_start(TX_RX) -> ra_ssie_write_sample x N
 * -> ra_ssie_stop -> ra_ssie_deinit. All MMIO is via the host
 * tests/mocks/ra_sim_mmap.c shim.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_ssie_regs.h"
#include "ra_err.h"
#include "ra_mstp.h"
#include "ra_sim_mmap.h"
#include "ra_ssie.h"
#include "unity_minimal.h"

typedef enum : uint8_t {
  k_test_ssie_app_channel      = 0U,
  k_test_ssie_app_bad_channel  = 4U,
  k_test_ssie_app_tx_threshold = 4U,
  k_test_ssie_app_rx_threshold = 4U,
  k_test_ssie_app_sample_count = 16U,
} test_ssie_app_chan_t;

static const uint32_t k_test_ssie_pattern[16] = {
  0x00000000U,
  0x30FB0000U,
  0x5A820000U,
  0x76410000U,
  0x7FFF0000U,
  0x76410000U,
  0x5A820000U,
  0x30FB0000U,
  0x00000000U,
  0xCF050000U,
  0xA57E0000U,
  0x89BF0000U,
  0x80010000U,
  0x89BF0000U,
  0xA57E0000U,
  0xCF050000U,
};

static ra_ssie_cfg_t default_cfg(void)
{
  ra_ssie_cfg_t c = {
    .role          = k_ra_ssie_role_master,
    .format        = k_ra_ssie_format_i2s,
    .data_word     = k_ra_ssie_dwl_16,
    .system_word   = k_ra_ssie_swl_32,
    .bclk_div      = k_ra_ssie_bclk_div_32,
    .use_gpt_clk   = false,
    .long_frame    = true,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = true,
    .tx_threshold  = (uint8_t)k_test_ssie_app_tx_threshold,
    .rx_threshold  = (uint8_t)k_test_ssie_app_rx_threshold,
  };
  return c;
}

static void reset_world(void)
{
  ra_sim_mmap_reset();
  (void)ra_mstp_init();
}

/**
 * @brief Golden bring-up: init + start + stream samples + stop.
 *
 * @par MC/DC:
 * Compound decision in app: ``init != ok || start != ok ||
 * write_sample != ok``. Three atomic conditions x N+1 = 4 vectors --
 * golden (this) + null cfg + bad channel + write bad channel.
 */
static void test_ssie_app_run_ok(void)
{
  reset_world();
  TEST_BEGIN("ssie_audio_loop: init + start + stream");
  const ra_ssie_cfg_t cfg = default_cfg();
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ssie_init((uint8_t)k_test_ssie_app_channel, &cfg));
  /* Seed SSISR.IIRQ so ra_ssie_start sees the channel as idle. On real
   * silicon SSISR.IIRQ self-asserts after the FIFO reset; the host
   * mock does not auto-toggle it. */
  ra_ssie((uint8_t)k_test_ssie_app_channel)->SSISR = (uint32_t)k_ra_ssie_mask_iirq;
  TEST_ASSERT_EQ((int)k_ra_ok,
                 (int)ra_ssie_start((uint8_t)k_test_ssie_app_channel, k_ra_ssie_dir_tx_rx));
  for (uint8_t i = 0U; i < (uint8_t)k_test_ssie_app_sample_count; i++) {
    TEST_ASSERT_EQ(
      (int)k_ra_ok,
      (int)ra_ssie_write_sample((uint8_t)k_test_ssie_app_channel, k_test_ssie_pattern[i]));
  }
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ssie_stop((uint8_t)k_test_ssie_app_channel));
  TEST_ASSERT_EQ((int)k_ra_ok, (int)ra_ssie_deinit((uint8_t)k_test_ssie_app_channel));
  TEST_END("ssie_audio_loop: init + start + stream");
}

/**
 * @brief NULL cfg rejected by ra_ssie_init.
 *
 * @par MC/DC:
 * Decision: ``cfg == nullptr``. One atomic condition x 2 vectors --
 * golden + this NULL.
 */
static void test_ssie_app_init_null(void)
{
  reset_world();
  TEST_BEGIN("ssie_audio_loop: NULL cfg rejected");
  TEST_ASSERT_EQ((int)k_ra_err_null_ptr,
                 (int)ra_ssie_init((uint8_t)k_test_ssie_app_channel, nullptr));
  TEST_END("ssie_audio_loop: NULL cfg rejected");
}

/**
 * @brief Bad channel rejected by ra_ssie_init.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra_ssie_max_channel``. One atomic condition
 * x 2 vectors -- in-range golden + this out-of-range.
 */
static void test_ssie_app_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("ssie_audio_loop: bad channel rejected");
  const ra_ssie_cfg_t cfg = default_cfg();
  TEST_ASSERT(ra_ssie_init((uint8_t)k_test_ssie_app_bad_channel, &cfg) != k_ra_ok);
  TEST_END("ssie_audio_loop: bad channel rejected");
}

/**
 * @brief Bad channel rejected by ra_ssie_write_sample.
 *
 * @par MC/DC:
 * Decision: ``channel < k_ra_ssie_max_channel`` for write_sample.
 * One atomic condition x 2 vectors -- in-range golden + this
 * out-of-range.
 */
static void test_ssie_app_write_bad_channel(void)
{
  reset_world();
  TEST_BEGIN("ssie_audio_loop: write bad channel rejected");
  TEST_ASSERT(ra_ssie_write_sample((uint8_t)k_test_ssie_app_bad_channel, 0xDEADBEEFU) != k_ra_ok);
  TEST_END("ssie_audio_loop: write bad channel rejected");
}

int main(void)
{
  test_ssie_app_run_ok();
  test_ssie_app_init_null();
  test_ssie_app_bad_channel();
  test_ssie_app_write_bad_channel();
  return 0;
}
