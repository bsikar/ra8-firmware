/**
 * @file test_fw_if_timer_ra8.c
 * @brief Vectors for the RA8 GPT adapter behind the neutral timer port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * Every vector drives the adapter through the public `fw_timer_*` facade, so
 * what is proven is that the facade and the adapter agree, not just that the
 * ops work in isolation. Register state is read back through the fake memory
 * map the GPT driver tests already use: channel 7 is the workhorse because a
 * channel other than 0 is what catches a start or stop landing on the wrong
 * counter. Every vector that opens a channel closes it, because which
 * channels are open is adapter state that outlives a single case.
 *
 * Nothing here proves counting on silicon; that needs the bench.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_timer.h"
#include "fw_if_timer_ra8.h"
#include "ra8_err.h"
#include "ra8_fake_mmap.h"
#include "ra8_gpt.h"
#include "ra8_gpt_regs.h"
#include "unity_minimal.h"

/** @brief Fixed inputs and the register values they should produce. */
typedef enum : uint32_t {
  k_test_ch         = 7U,           /**< Chip channel under test.   */
  k_test_ch_other   = 3U,           /**< A second channel.          */
  k_test_ch_bit     = 0x00000080UL, /**< CSTRT7 / CSTOP7.           */
  k_test_period     = 0x0001869FUL, /**< 99999: wrap point.         */
  k_test_period_new = 0x0000C34FUL, /**< 49999: changed wrap point. */
  k_test_count      = 0x00012345UL, /**< Count planted in GTCNT.    */
  k_test_gtcr_md    = 0x000F0000UL, /**< GTCR.MD field.             */
  k_test_md_oneshot = 0x00010000UL, /**< MD = saw one-shot.         */
  k_test_gtst_tcfpo = 0x00000040UL, /**< GTST.TCFPO: count wrapped. */
  k_test_gtst_other = 0x00000001UL, /**< GTST.TCFA: compare A hit.  */
} test_const_t;

/** @brief A handle bound to the adapter, failing the case if bind fails. */
static fw_timer_t bound(void)
{
  fw_timer_t tmr = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_ra8_bind(&tmr));
  return tmr;
}

/** @brief Shorthand for a channel by chip index. */
static fw_timer_ch_t ch_of(uint32_t index)
{
  return (fw_timer_ch_t){.index = (uint8_t)index};
}

static void test_caps_report_ten_32bit_channels(void)
{
  TEST_BEGIN("caps: ten 32-bit channels, one-shot yes, capture no");
  const fw_timer_t tmr  = bound();
  fw_timer_caps_t  caps = {};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_get_caps(&tmr, &caps));
  TEST_ASSERT_EQ(k_fw_timer_ra8_channel_count, caps.channel_count);
  TEST_ASSERT_EQ(k_fw_timer_ra8_counter_bits, caps.counter_bits);
  TEST_ASSERT_EQ(UINT32_MAX, caps.counter_max);
  TEST_ASSERT_EQ(true, caps.has_one_shot);
  TEST_ASSERT_EQ(false, caps.has_capture);
  TEST_END("caps: ten 32-bit channels, one-shot yes, capture no");
}

static void test_channels_past_ten_are_not_found(void)
{
  TEST_BEGIN("channel 10 and up are not found, unproven widths stay out");
  const fw_timer_t tmr = bound();
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 fw_timer_open(&tmr,
                               ch_of(k_fw_timer_ra8_channel_count),
                               k_fw_timer_mode_free_run,
                               k_test_period));
  TEST_END("channel 10 and up are not found, unproven widths stay out");
}

static void test_capture_is_refused(void)
{
  TEST_BEGIN("capture mode and capture_read are refused");
  const fw_timer_t tmr   = bound();
  uint32_t         count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_capture, k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_timer_capture_read(&tmr, ch_of(k_test_ch), &count));
  TEST_END("capture mode and capture_read are refused");
}

static void test_free_run_open_programs_and_stays_stopped(void)
{
  TEST_BEGIN("free-run open programs GTPR, saw mode, counter not started");
  ra8_fake_mmap_reset();
  const fw_timer_t               tmr = bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_free_run, k_test_period));
  TEST_ASSERT_EQ(k_test_period, reg->GTPR);
  TEST_ASSERT_EQ(0U, reg->GTCR & k_test_gtcr_md);
  TEST_ASSERT_EQ(0U, reg->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_END("free-run open programs GTPR, saw mode, counter not started");
}

static void test_one_shot_selects_one_shot_mode(void)
{
  TEST_BEGIN("one-shot open selects GTCR.MD saw one-shot");
  ra8_fake_mmap_reset();
  const fw_timer_t tmr = bound();
  TEST_ASSERT_EQ(
    k_ra8_ok,
    fw_timer_open(&tmr, ch_of(k_test_ch_other), k_fw_timer_mode_one_shot, k_test_period));
  TEST_ASSERT_EQ(k_test_md_oneshot, ra8_gpt((uint8_t)k_test_ch_other)->GTCR & k_test_gtcr_md);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch_other)));
  TEST_END("one-shot open selects GTCR.MD saw one-shot");
}

static void test_start_stop_hit_this_channel(void)
{
  TEST_BEGIN("start and stop write channel 7's own bit, and close stops it");
  ra8_fake_mmap_reset();
  const fw_timer_t               tmr = bound();
  volatile r_gpt_channel_regs_t* reg = ra8_gpt((uint8_t)k_test_ch);

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_free_run, k_test_period));
  reg->GTSTP = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_start(&tmr, ch_of(k_test_ch)));
  TEST_ASSERT_EQ(k_test_ch_bit, reg->GTSTR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_stop(&tmr, ch_of(k_test_ch)));
  TEST_ASSERT_EQ(k_test_ch_bit, reg->GTSTP);
  reg->GTSTP = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_ASSERT_EQ(k_test_ch_bit, reg->GTSTP);
  TEST_END("start and stop write channel 7's own bit, and close stops it");
}

static void test_read_and_set_period(void)
{
  TEST_BEGIN("read returns GTCNT; set_period while stopped lands in GTPR");
  ra8_fake_mmap_reset();
  const fw_timer_t               tmr   = bound();
  volatile r_gpt_channel_regs_t* reg   = ra8_gpt((uint8_t)k_test_ch);
  uint32_t                       count = 0U;

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_free_run, k_test_period));
  reg->GTCNT = k_test_count;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_read(&tmr, ch_of(k_test_ch), &count));
  TEST_ASSERT_EQ(k_test_count, count);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_set_period(&tmr, ch_of(k_test_ch), k_test_period_new));
  TEST_ASSERT_EQ(k_test_period_new, reg->GTPR);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_END("read returns GTCNT; set_period while stopped lands in GTPR");
}

static void test_take_wrap_reads_and_clears_tcfpo(void)
{
  TEST_BEGIN("take_wrap reports TCFPO once, clears only it, leaves other flags");
  ra8_fake_mmap_reset();
  const fw_timer_t               tmr     = bound();
  volatile r_gpt_channel_regs_t* reg     = ra8_gpt((uint8_t)k_test_ch);
  bool                           wrapped = true;

  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_one_shot, k_test_period));
  reg->GTST = k_test_gtst_other;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_take_wrap(&tmr, ch_of(k_test_ch), &wrapped));
  TEST_ASSERT_EQ(false, wrapped);
  TEST_ASSERT_EQ(k_test_gtst_other, reg->GTST);

  reg->GTST = k_test_gtst_tcfpo | k_test_gtst_other;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_take_wrap(&tmr, ch_of(k_test_ch), &wrapped));
  TEST_ASSERT_EQ(true, wrapped);
  TEST_ASSERT_EQ(k_test_gtst_other, reg->GTST);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_take_wrap(&tmr, ch_of(k_test_ch), &wrapped));
  TEST_ASSERT_EQ(false, wrapped);

  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_take_wrap(&tmr, ch_of(k_test_ch), &wrapped));
  TEST_END("take_wrap reports TCFPO once, clears only it, leaves other flags");
}

static void test_double_open_is_busy(void)
{
  TEST_BEGIN("opening an open channel is busy, not a second MSTP reference");
  ra8_fake_mmap_reset();
  const fw_timer_t tmr = bound();
  TEST_ASSERT_EQ(k_ra8_ok,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_free_run, k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_busy,
                 fw_timer_open(&tmr, ch_of(k_test_ch), k_fw_timer_mode_one_shot, k_test_period));
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_close(&tmr, ch_of(k_test_ch)));
  TEST_END("opening an open channel is busy, not a second MSTP reference");
}

static void test_ops_on_unopened_channel_refused(void)
{
  TEST_BEGIN("start, stop, read, set_period, close refuse an unopened channel");
  ra8_fake_mmap_reset();
  const fw_timer_t tmr   = bound();
  uint32_t         count = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_start(&tmr, ch_of(k_test_ch_other)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_stop(&tmr, ch_of(k_test_ch_other)));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_read(&tmr, ch_of(k_test_ch_other), &count));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state,
                 fw_timer_set_period(&tmr, ch_of(k_test_ch_other), k_test_period));
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_close(&tmr, ch_of(k_test_ch_other)));
  TEST_ASSERT_EQ(0U, ra8_gpt((uint8_t)k_test_ch_other)->GTSTR);
  TEST_END("start, stop, read, set_period, close refuse an unopened channel");
}

/** @brief Cases in run order; main walks this so it never grows. */
static void (*const s_test_roster[])(void) = {
  test_caps_report_ten_32bit_channels,
  test_channels_past_ten_are_not_found,
  test_capture_is_refused,
  test_free_run_open_programs_and_stays_stopped,
  test_one_shot_selects_one_shot_mode,
  test_start_stop_hit_this_channel,
  test_read_and_set_period,
  test_take_wrap_reads_and_clears_tcfpo,
  test_double_open_is_busy,
  test_ops_on_unopened_channel_refused,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
