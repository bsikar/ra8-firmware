/**
 * @file test_fw_if_timer.c
 * @brief Host vectors for the `fw_timer` port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The port has no chip adapter yet, so these vectors drive it through a fake
 * binding whose caps a vector sets per case. That is the right shape for what
 * is being proved: everything here is facade behaviour no chip can change --
 * the entry guards, the malformed-binding rejection, the caps snapshot taken
 * at bind, and above all the period range check, which is the reason this port
 * carries caps at all. A 16-bit backend handed a 32-bit period must come back
 * refused rather than truncating, and that is a vector, not a hope.
 *
 * What a chip adapter will owe is a separate mapping test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_timer.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief Counter widths the vectors below pin their expectations to. */
typedef enum : uint32_t {
  k_fake_max_16bit = 65535U,      /**< Widest 16-bit period. */
  k_fake_max_32bit = 4294967295U, /**< Widest 32-bit period. */
} fake_limit_t;

/** @brief Channel counts and widths used by the vectors. */
typedef enum : uint8_t {
  k_fake_channels    = 4U,  /**< Board instances the fake reports. */
  k_fake_bits_16     = 16U, /**< Narrow counter width.             */
  k_fake_bits_32     = 32U, /**< Wide counter width.               */
} fake_shape_t;

/** @brief What the fake binding was asked, so a vector can assert on it. */
typedef struct {
  fw_timer_caps_t caps;         /**< What get_caps reports.            */
  ra8_err_t       caps_err;     /**< Error get_caps reports.           */
  ra8_err_t       op_err;       /**< Error the forwarding ops report.  */
  uint32_t        counts;       /**< Value read / capture_read give.   */
  fw_timer_mode_t last_mode;    /**< Mode of the last open.            */
  uint32_t        last_period;  /**< Period of the last open / retune. */
  uint8_t         last_index;   /**< Channel of the last call.         */
  uint32_t        calls;        /**< Total forwarding ops entered.     */
} fake_state_t;

static fake_state_t g_fake;

static ra8_err_t fake_get_caps(void *ctx, fw_timer_caps_t *out) {
  fake_state_t *st = (fake_state_t *)ctx;
  *out             = st->caps;
  return st->caps_err;
}

static ra8_err_t fake_open(void *ctx, fw_timer_ch_t ch, fw_timer_mode_t mode, uint32_t period) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index  = ch.index;
  st->last_mode   = mode;
  st->last_period = period;
  return st->op_err;
}

static ra8_err_t fake_close(void *ctx, fw_timer_ch_t ch) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  return st->op_err;
}

static ra8_err_t fake_start(void *ctx, fw_timer_ch_t ch) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  return st->op_err;
}

static ra8_err_t fake_stop(void *ctx, fw_timer_ch_t ch) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  return st->op_err;
}

static ra8_err_t fake_read(void *ctx, fw_timer_ch_t ch, uint32_t *out_counts) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  *out_counts    = st->counts;
  return st->op_err;
}

static ra8_err_t fake_set_period(void *ctx, fw_timer_ch_t ch, uint32_t period) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index  = ch.index;
  st->last_period = period;
  return st->op_err;
}

static ra8_err_t fake_capture_read(void *ctx, fw_timer_ch_t ch, uint32_t *out_counts) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_index = ch.index;
  *out_counts    = st->counts;
  return st->op_err;
}

static const fw_timer_iface_t g_fake_iface = {
  .get_caps     = fake_get_caps,
  .open         = fake_open,
  .close        = fake_close,
  .start        = fake_start,
  .stop         = fake_stop,
  .read         = fake_read,
  .set_period   = fake_set_period,
  .capture_read = fake_capture_read,
};

static const fw_timer_ch_t k_ch0 = {.index = 0U};

/** @brief Reset the fake to a wide, fully capable backend. */
static void internal_fake_reset_32bit(void) {
  const fake_state_t fresh = {
    .caps =
      {
        .channel_count = (uint8_t)k_fake_channels,
        .counter_bits  = (uint8_t)k_fake_bits_32,
        .counter_max   = (uint32_t)k_fake_max_32bit,
        .has_capture   = true,
        .has_one_shot  = true,
      },
  };
  g_fake = fresh;
}

/** @brief Reset the fake to a narrow backend with neither optional mode. */
static void internal_fake_reset_16bit(void) {
  const fake_state_t fresh = {
    .caps =
      {
        .channel_count = (uint8_t)k_fake_channels,
        .counter_bits  = (uint8_t)k_fake_bits_16,
        .counter_max   = (uint32_t)k_fake_max_16bit,
        .has_capture   = false,
        .has_one_shot  = false,
      },
  };
  g_fake = fresh;
}

static void test_bind_rejects_null_and_incomplete(void) {
  TEST_BEGIN("bind rejects a NULL handle, NULL ops, and any single unset op");

  internal_fake_reset_32bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(NULL, &g_fake_iface, &g_fake));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, NULL, &g_fake));
  TEST_ASSERT(!tmr.bound);

  /* Each op is cleared on its own, so adding a ninth cannot be forgotten
   * here without this vector going quiet about it. */
  fw_timer_iface_t partial = g_fake_iface;
  partial.get_caps         = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.open             = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.close            = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.start            = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.stop             = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.read             = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.set_period       = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  partial                  = g_fake_iface;
  partial.capture_read     = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_bind(&tmr, &partial, &g_fake));
  TEST_ASSERT(!tmr.bound);

  TEST_END("bind rejects a NULL handle, NULL ops, and any single unset op");
}

static void test_bind_refuses_unusable_caps(void) {
  TEST_BEGIN("a backend reporting a zero counter width or limit is refused at bind");

  internal_fake_reset_32bit();
  g_fake.caps.counter_bits = 0U;
  fw_timer_t tmr           = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));
  TEST_ASSERT(!tmr.bound);

  internal_fake_reset_32bit();
  g_fake.caps.counter_max = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));
  TEST_ASSERT(!tmr.bound);

  /* A get_caps that fails propagates its own error rather than being
   * relabelled as a state problem. */
  internal_fake_reset_32bit();
  g_fake.caps_err = k_ra8_err_timeout;
  TEST_ASSERT_EQ(k_ra8_err_timeout, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));
  TEST_ASSERT(!tmr.bound);

  TEST_END("a backend reporting a zero counter width or limit is refused at bind");
}

static void test_unbound_handle_is_refused(void) {
  TEST_BEGIN("every entry point refuses a zeroed handle rather than jumping through it");

  const fw_timer_t zeroed = {0};
  uint32_t         counts = 7U;
  fw_timer_caps_t  caps   = {.channel_count = 9U};

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_get_caps(&zeroed, &caps));
  TEST_ASSERT_EQ(0U, caps.channel_count);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized,
                 fw_timer_open(&zeroed, k_ch0, k_fw_timer_mode_free_run, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_close(&zeroed, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_start(&zeroed, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_stop(&zeroed, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_read(&zeroed, k_ch0, &counts));
  TEST_ASSERT_EQ(0U, counts);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_set_period(&zeroed, k_ch0, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_timer_capture_read(&zeroed, k_ch0, &counts));

  /* A NULL handle is an argument error, not a state error. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_start(NULL, k_ch0));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_read(NULL, k_ch0, &counts));

  TEST_END("every entry point refuses a zeroed handle rather than jumping through it");
}

static void test_caps_snapshot_survives_a_lying_backend(void) {
  TEST_BEGIN("caps come from the bind-time snapshot, not a fresh call to the backend");

  internal_fake_reset_16bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  /* The backend now claims to be wide. The handle must not believe it:
   * limits that can move underneath a caller are not limits. */
  g_fake.caps.counter_max  = (uint32_t)k_fake_max_32bit;
  g_fake.caps.counter_bits = (uint8_t)k_fake_bits_32;

  fw_timer_caps_t caps = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_get_caps(&tmr, &caps));
  TEST_ASSERT_EQ((uint32_t)k_fake_max_16bit, caps.counter_max);
  TEST_ASSERT_EQ((uint8_t)k_fake_bits_16, caps.counter_bits);
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_free_run,
                               (uint32_t)k_fake_max_16bit + 1U));

  TEST_END("caps come from the bind-time snapshot, not a fresh call to the backend");
}

static void test_period_is_range_checked_against_the_counter(void) {
  TEST_BEGIN("a period wider than the counter is refused instead of truncating");

  internal_fake_reset_16bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  /* The whole reason this port carries caps: a 16-bit backend handed a
   * 32-bit period would otherwise keep the low half and run fast. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_free_run, 100000U));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_free_run, 0U));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_free_run,
                                         (uint32_t)k_fake_max_16bit));
  TEST_ASSERT_EQ((uint32_t)k_fake_max_16bit, g_fake.last_period);

  /* A retune can overflow exactly as an open can, so it is checked the same. */
  TEST_ASSERT_EQ(k_ra8_err_out_of_range, fw_timer_set_period(&tmr, k_ch0, 70000U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_set_period(&tmr, k_ch0, 0U));
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_set_period(&tmr, k_ch0, 1000U));
  TEST_ASSERT_EQ(1000U, g_fake.last_period);

  TEST_END("a period wider than the counter is refused instead of truncating");
}

static void test_mode_is_enumerated_and_capability_checked(void) {
  TEST_BEGIN("an unenumerated mode is an argument error, an absent one is not supported");

  internal_fake_reset_16bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  /* A zeroed mode is the shape a caller gets by forgetting to fill one in. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_none, 100U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 fw_timer_open(&tmr, k_ch0, (fw_timer_mode_t)K_FW_TIMER_MODE_COUNT, 100U));

  /* This backend declared neither optional mode, so both are declined and
   * neither reaches it. */
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_capture, 100U));
  TEST_ASSERT_EQ(k_ra8_err_not_supported,
                 fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_one_shot, 100U));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  /* Free-run is not optional and gets through. */
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_open(&tmr, k_ch0, k_fw_timer_mode_free_run, 100U));
  TEST_ASSERT_EQ(k_fw_timer_mode_free_run, g_fake.last_mode);

  TEST_END("an unenumerated mode is an argument error, an absent one is not supported");
}

static void test_channel_index_is_checked_against_the_board_count(void) {
  TEST_BEGIN("a channel the board does not carry is not_found on every operation");

  internal_fake_reset_32bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  const fw_timer_ch_t past = {.index = (uint8_t)k_fake_channels};
  uint32_t            counts = 5U;
  TEST_ASSERT_EQ(k_ra8_err_not_found,
                 fw_timer_open(&tmr, past, k_fw_timer_mode_free_run, 100U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_close(&tmr, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_start(&tmr, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_stop(&tmr, past));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_read(&tmr, past, &counts));
  TEST_ASSERT_EQ(0U, counts);
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_set_period(&tmr, past, 100U));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_capture_read(&tmr, past, &counts));
  TEST_ASSERT_EQ(0U, g_fake.calls);

  /* A board with no timers makes every index absent, index zero included. */
  internal_fake_reset_32bit();
  g_fake.caps.channel_count = 0U;
  fw_timer_t empty          = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&empty, &g_fake_iface, &g_fake));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_timer_start(&empty, k_ch0));

  TEST_END("a channel the board does not carry is not_found on every operation");
}

static void test_reads_zero_their_output_and_propagate(void) {
  TEST_BEGIN("read and capture_read zero the output on failure and pass the value on success");

  internal_fake_reset_32bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  g_fake.counts   = 12345U;
  uint32_t counts = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_read(&tmr, k_ch0, &counts));
  TEST_ASSERT_EQ(12345U, counts);
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_capture_read(&tmr, k_ch0, &counts));
  TEST_ASSERT_EQ(12345U, counts);

  /* A backend failure leaves nothing stale behind for the caller to use. */
  g_fake.op_err = k_ra8_err_would_block;
  counts        = 999U;
  TEST_ASSERT_EQ(k_ra8_err_would_block, fw_timer_capture_read(&tmr, k_ch0, &counts));
  TEST_ASSERT_EQ(0U, counts);
  counts = 999U;
  TEST_ASSERT_EQ(k_ra8_err_would_block, fw_timer_read(&tmr, k_ch0, &counts));
  TEST_ASSERT_EQ(0U, counts);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_read(&tmr, k_ch0, NULL));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_timer_capture_read(&tmr, k_ch0, NULL));

  TEST_END("read and capture_read zero the output on failure and pass the value on success");
}

static void test_capture_without_support_never_reaches_the_backend(void) {
  TEST_BEGIN("capture_read on a backend without capture is refused at the facade");

  internal_fake_reset_16bit();
  fw_timer_t tmr = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr, &g_fake_iface, &g_fake));

  /* Refused here, so a caller never has to tell "this backend cannot" apart
   * from "no edge has arrived yet". */
  uint32_t counts = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_timer_capture_read(&tmr, k_ch0, &counts));
  TEST_ASSERT_EQ(0U, counts);
  TEST_ASSERT_EQ(0U, g_fake.calls);

  TEST_END("capture_read on a backend without capture is refused at the facade");
}

static void test_context_reaches_every_op(void) {
  TEST_BEGIN("the binding context is handed back to every op, so no file-scope state");

  internal_fake_reset_32bit();
  fake_state_t first  = g_fake;
  fake_state_t second = g_fake;

  fw_timer_t tmr_a = {0};
  fw_timer_t tmr_b = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr_a, &g_fake_iface, &first));
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_bind(&tmr_b, &g_fake_iface, &second));

  const fw_timer_ch_t ch2 = {.index = 2U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_start(&tmr_a, k_ch0));
  TEST_ASSERT_EQ(k_ra8_ok, fw_timer_stop(&tmr_b, ch2));
  TEST_ASSERT_EQ(1U, first.calls);
  TEST_ASSERT_EQ(1U, second.calls);
  TEST_ASSERT_EQ(0U, first.last_index);
  TEST_ASSERT_EQ(2U, second.last_index);

  TEST_END("the binding context is handed back to every op, so no file-scope state");
}

int main(void) {
  test_bind_rejects_null_and_incomplete();
  test_bind_refuses_unusable_caps();
  test_unbound_handle_is_refused();
  test_caps_snapshot_survives_a_lying_backend();
  test_period_is_range_checked_against_the_counter();
  test_mode_is_enumerated_and_capability_checked();
  test_channel_index_is_checked_against_the_board_count();
  test_reads_zero_their_output_and_propagate();
  test_capture_without_support_never_reaches_the_backend();
  test_context_reaches_every_op();
  return 0;
}
