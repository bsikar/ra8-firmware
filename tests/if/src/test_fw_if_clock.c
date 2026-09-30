/**
 * @file test_fw_if_clock.c
 * @brief Host vectors for the `fw_clock` intent port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The port has no chip adapter yet, so these vectors drive it through a fake
 * binding that answers from a small table. That is not a weaker test than one
 * against real silicon would be: everything this file proves -- the entry
 * guards, the malformed-binding rejection, the zero-rate refusal, and the
 * comparison in ::fw_clock_require -- is facade behaviour that no chip can
 * change. What a chip adapter will owe is a separate mapping test.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/** @brief What the fake binding was asked, so a vector can assert on it. */
typedef struct {
  uint32_t          rate_hz;     /**< Rate rate_for reports.         */
  ra8_err_t         rate_err;    /**< Error rate_for reports.        */
  ra8_err_t         gate_err;    /**< Error set_gate reports.        */
  bool              present;     /**< What has_module reports.       */
  bool              last_on;     /**< The `on` of the last set_gate. */
  fw_clock_module_t last_module; /**< Module of the last call.       */
  uint32_t          calls;       /**< Total ops entered.             */
} fake_state_t;

static fake_state_t g_fake;

static ra8_err_t fake_rate_for(void *ctx, fw_clock_module_t module, uint32_t *out_hz) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_module = module;
  *out_hz         = st->rate_hz;
  return st->rate_err;
}

static ra8_err_t fake_set_gate(void *ctx, fw_clock_module_t module, bool on) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_module = module;
  st->last_on     = on;
  return st->gate_err;
}

static ra8_err_t fake_has_module(void *ctx, fw_clock_module_t module, bool *out_present) {
  fake_state_t *st = (fake_state_t *)ctx;
  st->calls += 1U;
  st->last_module = module;
  *out_present    = st->present;
  return k_ra8_ok;
}

static const fw_clock_iface_t g_fake_iface = {
    .rate_for   = fake_rate_for,
    .set_gate   = fake_set_gate,
    .has_module = fake_has_module,
};

/** @brief A bound handle over a reset fake, the starting point of most vectors. */
static fw_clock_t bound_fake(uint32_t rate_hz) {
  g_fake          = (fake_state_t){0};
  g_fake.rate_hz  = rate_hz;
  g_fake.rate_err = k_ra8_ok;
  g_fake.gate_err = k_ra8_ok;
  g_fake.present  = true;

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_bind(&clk, &g_fake_iface, &g_fake));
  return clk;
}

static const fw_clock_module_t k_uart0 = {
    .kind  = k_fw_clock_module_uart,
    .index = 0U,
};

static void test_bind_rejects_null_and_incomplete(void) {
  TEST_BEGIN("bind rejects a NULL handle, NULL ops, and a half-filled ops struct");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_bind(NULL, &g_fake_iface, NULL));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_bind(&clk, NULL, NULL));
  TEST_ASSERT(!clk.bound);

  /* A binding that leaves an op NULL is malformed. Each of the three is
   * rejected on its own so a future fourth op cannot be forgotten here. */
  fw_clock_iface_t partial = g_fake_iface;
  partial.rate_for         = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_bind(&clk, &partial, NULL));
  partial                  = g_fake_iface;
  partial.set_gate         = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_bind(&clk, &partial, NULL));
  partial                  = g_fake_iface;
  partial.has_module       = NULL;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_bind(&clk, &partial, NULL));

  TEST_END("bind rejects a NULL handle, NULL ops, and a half-filled ops struct");
}

static void test_unbound_handle_is_refused(void) {
  TEST_BEGIN("every entry point refuses a zeroed handle rather than jumping through it");

  const fw_clock_t zeroed  = {0};
  uint32_t         hz      = 7U;
  bool             present = true;

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_clock_rate_for(&zeroed, k_uart0, &hz));
  TEST_ASSERT_EQ(0U, hz);
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_clock_require(&zeroed, k_uart0, 1U, &hz));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_clock_enable(&zeroed, k_uart0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_clock_disable(&zeroed, k_uart0));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, fw_clock_has_module(&zeroed, k_uart0, &present));
  TEST_ASSERT(!present);

  /* A NULL handle is an argument error, not a state error. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_rate_for(NULL, k_uart0, &hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_enable(NULL, k_uart0));

  TEST_END("every entry point refuses a zeroed handle rather than jumping through it");
}

static void test_module_kind_is_range_checked(void) {
  TEST_BEGIN("a zeroed module and an out-of-range kind are both rejected");

  fw_clock_t clk = bound_fake(48000000U);
  uint32_t   hz  = 0U;

  const fw_clock_module_t none = {.kind = k_fw_clock_module_none, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_rate_for(&clk, none, &hz));

  const fw_clock_module_t past = {
      .kind  = (fw_clock_module_kind_t)K_FW_CLOCK_MODULE_KIND_COUNT,
      .index = 0U,
  };
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_rate_for(&clk, past, &hz));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_enable(&clk, past));

  /* None of those four reached the binding. */
  TEST_ASSERT_EQ(0U, g_fake.calls);

  /* The last enumerator is inside the range, so it does reach it. */
  const fw_clock_module_t last = {.kind = k_fw_clock_module_memory, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk, last, &hz));
  TEST_ASSERT_EQ(1U, g_fake.calls);

  TEST_END("a zeroed module and an out-of-range kind are both rejected");
}

static void test_rate_for_passes_module_through_and_zeroes_on_failure(void) {
  TEST_BEGIN("rate_for hands the module to the binding and zeroes its output on every failure");

  fw_clock_t clk = bound_fake(120000000U);
  uint32_t   hz  = 0U;

  const fw_clock_module_t spi2 = {.kind = k_fw_clock_module_spi, .index = 2U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk, spi2, &hz));
  TEST_ASSERT_EQ(120000000U, hz);
  TEST_ASSERT_EQ(k_fw_clock_module_spi, g_fake.last_module.kind);
  TEST_ASSERT_EQ(2U, g_fake.last_module.index);

  /* A NULL output is refused before the binding is called. */
  const uint32_t calls_before = g_fake.calls;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_rate_for(&clk, spi2, NULL));
  TEST_ASSERT_EQ(calls_before, g_fake.calls);

  /* A binding error propagates verbatim, with the output cleared. */
  hz               = 99U;
  g_fake.rate_err  = k_ra8_err_not_found;
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_rate_for(&clk, spi2, &hz));
  TEST_ASSERT_EQ(0U, hz);

  TEST_END("rate_for hands the module to the binding and zeroes its output on every failure");
}

static void test_zero_rate_from_binding_is_a_state_error(void) {
  TEST_BEGIN("a binding reporting success with a zero rate is refused at the seam");

  fw_clock_t clk = bound_fake(0U);
  uint32_t   hz  = 5U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_clock_rate_for(&clk, k_uart0, &hz));
  TEST_ASSERT_EQ(0U, hz);

  /* The same refusal reaches a caller going through require, so nobody
   * divides by it. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_state, fw_clock_require(&clk, k_uart0, 1U, &hz));

  TEST_END("a binding reporting success with a zero rate is refused at the seam");
}

static void test_require_compares_and_keeps_the_rate(void) {
  TEST_BEGIN("require passes at or above the floor and still reports the rate when below");

  fw_clock_t clk = bound_fake(24000000U);
  uint32_t   hz  = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_require(&clk, k_uart0, 1000000U, &hz));
  TEST_ASSERT_EQ(24000000U, hz);

  /* Exactly at the floor is met: the contract says at or above. */
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_require(&clk, k_uart0, 24000000U, &hz));
  TEST_ASSERT_EQ(24000000U, hz);

  /* One hertz above it is not, and the actual rate is still written so the
   * driver can report what it found. */
  hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_require(&clk, k_uart0, 24000001U, &hz));
  TEST_ASSERT_EQ(24000000U, hz);

  TEST_END("require passes at or above the floor and still reports the rate when below");
}

static void test_require_rejects_a_zero_floor(void) {
  TEST_BEGIN("a floor of zero is an argument error, not a vacuous pass");

  fw_clock_t clk = bound_fake(24000000U);
  uint32_t   hz  = 3U;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_require(&clk, k_uart0, 0U, &hz));
  TEST_ASSERT_EQ(0U, hz);
  TEST_ASSERT_EQ(0U, g_fake.calls);

  /* And it is rejected before the handle is even looked at, so a NULL output
   * alongside it cannot be dereferenced. */
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_require(&clk, k_uart0, 0U, NULL));

  TEST_END("a floor of zero is an argument error, not a vacuous pass");
}

static void test_gating_carries_the_on_flag_and_the_error(void) {
  TEST_BEGIN("enable and disable differ only in the flag, and a declined gate propagates");

  fw_clock_t clk = bound_fake(48000000U);

  const fw_clock_module_t cam = {.kind = k_fw_clock_module_camera, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_enable(&clk, cam));
  TEST_ASSERT(g_fake.last_on);
  TEST_ASSERT_EQ(k_fw_clock_module_camera, g_fake.last_module.kind);

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_disable(&clk, cam));
  TEST_ASSERT(!g_fake.last_on);

  /* A binding that does not gate this module says so; the facade does not
   * translate it into something softer. */
  g_fake.gate_err = k_ra8_err_not_supported;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_enable(&clk, cam));

  TEST_END("enable and disable differ only in the flag, and a declined gate propagates");
}

static void test_has_module_reports_absence_without_an_error(void) {
  TEST_BEGIN("a module the board does not carry is a false answer, not a failure");

  fw_clock_t clk  = bound_fake(48000000U);
  bool       seen = false;

  const fw_clock_module_t eth = {.kind = k_fw_clock_module_ethernet, .index = 3U};
  g_fake.present              = false;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, eth, &seen));
  TEST_ASSERT(!seen);

  g_fake.present = true;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, eth, &seen));
  TEST_ASSERT(seen);
  TEST_ASSERT_EQ(3U, g_fake.last_module.index);

  /* A NULL output is refused before the binding runs. */
  const uint32_t calls_before = g_fake.calls;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_has_module(&clk, eth, NULL));
  TEST_ASSERT_EQ(calls_before, g_fake.calls);

  TEST_END("a module the board does not carry is a false answer, not a failure");
}

static void test_context_reaches_every_op(void) {
  TEST_BEGIN("the binding context is handed back to all three ops, so no file-scope state");

  fake_state_t     first  = {.rate_hz = 1000U, .present = true};
  fake_state_t     second = {.rate_hz = 2000U, .present = true};
  fw_clock_t       clk_a  = {0};
  fw_clock_t       clk_b  = {0};
  uint32_t         hz     = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_bind(&clk_a, &g_fake_iface, &first));
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_bind(&clk_b, &g_fake_iface, &second));

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk_a, k_uart0, &hz));
  TEST_ASSERT_EQ(1000U, hz);
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk_b, k_uart0, &hz));
  TEST_ASSERT_EQ(2000U, hz);

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_enable(&clk_b, k_uart0));
  TEST_ASSERT(second.last_on);
  TEST_ASSERT(!first.last_on);
  TEST_ASSERT_EQ(1U, first.calls);
  TEST_ASSERT_EQ(2U, second.calls);

  TEST_END("the binding context is handed back to all three ops, so no file-scope state");
}

int main(void) {
  test_bind_rejects_null_and_incomplete();
  test_unbound_handle_is_refused();
  test_module_kind_is_range_checked();
  test_rate_for_passes_module_through_and_zeroes_on_failure();
  test_zero_rate_from_binding_is_a_state_error();
  test_require_compares_and_keeps_the_rate();
  test_require_rejects_a_zero_floor();
  test_gating_carries_the_on_flag_and_the_error();
  test_has_module_reports_absence_without_an_error();
  test_context_reaches_every_op();
  return 0;
}
