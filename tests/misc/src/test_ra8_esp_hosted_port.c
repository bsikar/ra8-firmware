/**
 * @file test_ra8_esp_hosted_port.c
 * @brief Host tests for the esp-hosted port's bring-up validation.
 *
 * @par Tag
 * [Ring 7 / TEST] {World: NS}
 *
 * @details
 * Covers the two predicates ``ra8_esp_hosted_port.c`` runs before it
 * touches any hardware: the configuration check and the pin-map check.
 * Both are promoted out of `static` precisely so their rejection paths
 * are reachable from here -- the shipped pin map is a set of compile-time
 * constants and would otherwise exercise exactly one path, which is the
 * same as being unverified.
 *
 * The bring-up sequence itself is not driven here: it creates ThreadX
 * pools and opens an SCI channel, both of which belong to the slice tests
 * that own those seams. Its teardown counterpart contributes one decision
 * that is not a slice's -- the "first error wins" fold -- and that is
 * promoted for the same reason as the two predicates, so it is driven
 * directly rather than left to a board.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_esp_hosted_port.h"
#include "ra8_esp_hosted_port_internal.h"
#include "ra8_port_constants.h"
#include "unity_minimal.h"

/**
 * @enum t_port_channel_t
 * @brief The two SCI channel numbers that straddle the validator's bound.
 *
 * @details
 * ``priv_ra8_esp_hosted_port_cfg_check`` rejects any channel at or above the RA8D2
 * SCI channel count, so the boundary is only proven by a pair of vectors one
 * step apart: the last accepted channel and the first rejected one. Naming
 * both makes the adjacency visible, which a bare ``9U`` / ``10U`` pair beside
 * two different expected results does not.
 *
 * @invariant ::k_t_port_channel_first_bad is exactly one greater than
 *            ::k_t_port_channel_last_ok, so the two vectors remain adjacent
 *            and continue to pin the bound rather than merely sampling it.
 *
 * @par Example:
 * @code
 * cfg.sci_channel = (uint8_t)k_t_port_channel_first_bad;
 * @endcode
 *
 * @see priv_ra8_esp_hosted_port_cfg_check
 * @since 0.1.0
 */
typedef enum : uint8_t {
  k_t_port_channel_last_ok   = 9U,  /**< Highest channel the validator accepts. */
  k_t_port_channel_first_bad = 10U, /**< Lowest channel the validator rejects.  */
} t_port_channel_t;

/**
 * @var s_t_good_cfg
 * @brief A configuration every field of which is inside its contract.
 * @details The baseline the rejection vectors vary one field away from,
 * so each vector differs from the control in exactly one condition.
 * @note Read-only.
 * @warning Keep every field legal; the vectors assume it is the control.
 * @since 0.1.0
 */
static const ra8_esp_hosted_port_cfg_t s_t_good_cfg = {
  .pclk_hz      = 100000000U,
  .sck_hz       = 5000000U,
  .edge_poll_ms = 2U,
  .sci_channel  = 2U,
};

/**
 * @test ra8_esp_hosted_port_cfg_check_rejects_each_field
 *
 * @par MC/DC:
 * Decision: the four-condition rejection
 * `(pclk_hz == 0) || (sck_hz == 0) || (edge_poll_ms == 0) || (sci_channel >= 10)`.
 * - Vector 1: all four legal                       -> k_ra8_ok (control)
 * - Vector 2: pclk_hz = 0, rest legal              -> reject (varies pclk_hz)
 * - Vector 3: sck_hz = 0, rest legal               -> reject (varies sck_hz)
 * - Vector 4: edge_poll_ms = 0, rest legal         -> reject (varies edge_poll_ms)
 * - Vector 5: sci_channel = ::k_t_port_channel_first_bad (10), rest legal
 *   -> reject (varies sci_channel)
 * Vectors 1+2 prove pclk_hz independently affects the outcome; 1+3, 1+4 and
 * 1+5 do the same for the other three. N+1 = 5 vectors for N=4: minimal
 * MC/DC. The null-pointer guard is a separate one-condition decision and
 * takes the two vectors below it.
 *
 * @pre None; the predicate touches no hardware.
 * @post No module state is modified by any vector.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify cfg check rejects each field.
 * @details Drives the host model through cfg check rejects each field and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_cfg_check_rejects_each_field(void)
{
  TEST_BEGIN("esp-hosted port cfg validation");

  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_ra8_esp_hosted_port_cfg_check(nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_cfg_check(&s_t_good_cfg));

  ra8_esp_hosted_port_cfg_t cfg = s_t_good_cfg;
  cfg.pclk_hz                   = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_cfg_check(&cfg));

  cfg        = s_t_good_cfg;
  cfg.sck_hz = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_cfg_check(&cfg));

  cfg              = s_t_good_cfg;
  cfg.edge_poll_ms = 0U;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_cfg_check(&cfg));

  cfg             = s_t_good_cfg;
  cfg.sci_channel = (uint8_t)k_t_port_channel_first_bad;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_cfg_check(&cfg));

  cfg             = s_t_good_cfg;
  cfg.sci_channel = (uint8_t)k_t_port_channel_last_ok;
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_cfg_check(&cfg));

  TEST_END("esp-hosted port cfg validation");
}

/**
 * @test ra8_esp_hosted_port_pins_check_values_rejects_duplicates
 *
 * @par MC/DC:
 * Decision: the three-condition distinctness test
 * `(cs == hs) || (cs == dr) || (hs == dr)` in
 * `port/esp-hosted/src/ra8_esp_hosted_port.c@priv_ra8_esp_hosted_port_pins_check_values`.
 * - Vector 1: cs=0x0804 hs=0x040C dr=0x0006, all distinct -> accept (control)
 * - Vector 2: cs=0x0804 hs=0x0804 dr=0x0006              -> reject (varies cs==hs)
 * - Vector 3: cs=0x0804 hs=0x040C dr=0x0804              -> reject (varies cs==dr)
 * - Vector 4: cs=0x0804 hs=0x040C dr=0x040C              -> reject (varies hs==dr)
 * Vectors 1+2, 1+3 and 1+4 each prove one comparison independently affects
 * the outcome. N+1 = 4 vectors for N=3: minimal MC/DC.
 *
 * @pre None; the predicate touches no hardware.
 * @post No module state is modified by any vector.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify pins check rejects duplicates.
 * @details Drives the host model through pins check rejects duplicates and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_pins_check_rejects_duplicates(void)
{
  TEST_BEGIN("esp-hosted pin-map distinctness");

  const uint16_t cs = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_4);
  const uint16_t hs = (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_12);
  const uint16_t dr = (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_6);

  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_pins_check_values(cs, hs, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_pins_check_values(cs, cs, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_pins_check_values(cs, hs, cs));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_pins_check_values(cs, hs, hs));

  TEST_END("esp-hosted pin-map distinctness");
}

/**
 * @test ra8_esp_hosted_port_pins_check_values_rejects_out_of_range
 *
 * @par MC/DC:
 * Decision: the two-condition range test inside
 * `port/esp-hosted/src/ra8_esp_hosted_port.c@internal_pin_in_range`,
 * `(port <= k_ra8_port_max) && (pin <= k_ra8_pin_max)`, applied in turn to
 * each of the three signals.
 * - Vector 1: port 8, pin 4 -- both in range        -> accept (control)
 * - Vector 2: port 15, pin 4 -- port out of range   -> reject (varies port)
 * - Vector 3: port 8, pin 16 -- pin out of range    -> reject (varies pin)
 * Vectors 1+2 prove the port half independently affects the outcome; 1+3
 * prove the same for the pin half. N+1 = 3 vectors for N=2: minimal MC/DC.
 * The no-pin sentinel decodes to port 255 / pin 255 and is covered by the
 * same decision, which is why it needs no separate comparison in the code.
 * Each of the three signals is driven through the rejection so no call site
 * of the predicate is left unexercised.
 *
 * @pre None; the predicate touches no hardware.
 * @post No module state is modified by any vector.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify pins check rejects out of range.
 * @details Drives the host model through pins check rejects out of range and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_pins_check_rejects_out_of_range(void)
{
  TEST_BEGIN("esp-hosted pin-map range");

  const uint16_t cs = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_4);
  const uint16_t hs = (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_12);
  const uint16_t dr = (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_6);

  const uint16_t bad_port = (uint16_t)((15U << 8U) | 4U);
  const uint16_t bad_pin  = (uint16_t)((8U << 8U) | 16U);
  const uint16_t none     = (uint16_t)k_ra8_pin_none;

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(bad_port, hs, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(bad_pin, hs, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(cs, bad_port, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(cs, bad_pin, dr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(cs, hs, bad_port));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_port_pins_check_values(cs, hs, bad_pin));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_port_pins_check_values(none, hs, dr));

  TEST_END("esp-hosted pin-map range");
}

/**
 * @test ra8_esp_hosted_port_shipped_pin_map_is_consistent
 *
 * @par MC/DC:
 * Single-vector assertion over the compiled-in map, which is what makes
 * the pin header's stated invariants a build-time-checked claim rather
 * than a comment. The rejection vectors for the same decision live in the
 * two tests above, which drive the predicate's parameterised form.
 *
 * @pre The pin map header describes the EK-RA8D2.
 * @post No module state is modified.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify shipped pin map is consistent.
 * @details Drives the host model through shipped pin map is consistent and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_shipped_pin_map_is_consistent(void)
{
  TEST_BEGIN("esp-hosted shipped pin map");

  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_pins_check());
  TEST_ASSERT(!ra8_esp_hosted_port_is_ready());
  TEST_ASSERT(!ra8_esp_hosted_port_rx_pending());

  /* The reset net is legitimately unwired until the harness supplies one,
     so it is the one signal the consistency check must NOT demand. */
  TEST_ASSERT_EQ(k_ra8_pin_none, k_ra8_esp_hosted_pin_reset);

  TEST_END("esp-hosted shipped pin map");
}

/**
 * @test ra8_esp_hosted_port_deinit_refuses_when_never_up
 *
 * @par MC/DC:
 * Decision: the one-condition guard `if (!ready)`. Two vectors are needed
 * for one condition; only the false-ready arm is reachable without a board,
 * so the true arm is proved by the slice tests that bring the port up
 * against the ThreadX shim. Asserting the refusal here is what stops a
 * double teardown from being silently accepted.
 *
 * @pre The port has never been initialised in this process.
 * @post The port still reports not ready.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify deinit refuses when never up.
 * @details Drives the host model through deinit refuses when never up and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_deinit_refuses_when_never_up(void)
{
  TEST_BEGIN("esp-hosted teardown guard");

  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_esp_hosted_port_deinit());
  TEST_ASSERT(!ra8_esp_hosted_port_is_ready());

  TEST_END("esp-hosted teardown guard");
}

/**
 * @test ra8_esp_hosted_port_first_error_keeps_the_earliest_failure
 *
 * @par MC/DC:
 * Decision: `(first == k_ra8_ok) && (next != k_ra8_ok)` in
 * `port/esp-hosted/src/ra8_esp_hosted_port.c@priv_ra8_esp_hosted_port_first_error`
 * (2 conditions).
 * - Vector 1: first=ok,        next=ok        -> true, false -> false; the
 *   sequence stays clean (control)
 * - Vector 2: first=spi_error, next=ok        -> false        -> false; the
 *   earlier failure survives (varies the first condition)
 * - Vector 3: first=ok,        next=rtos_error -> true, true  -> true; the
 *   later failure is adopted (varies the second condition)
 * Vectors 1+3 prove the ``next`` comparison independently affects the
 * outcome; 2+3 prove the same for the ``first`` comparison, since the
 * ``next`` comparison is true in both. N+1 = 3 vectors for N=2: minimal
 * MC/DC. The fourth combination (first and next both errors) is asserted as
 * well: it must keep the earlier one, which is the property the unwind path
 * depends on and the only way a later error could mask an earlier one.
 *
 * This fold is promoted out of ``internal_unwind`` precisely so these
 * vectors exist: the unwind path itself runs only after a successful
 * bring-up, which needs an SCI channel and ThreadX pools -- a board.
 *
 * @pre None; the fold touches no hardware and no module state.
 * @post No module state is modified by any vector.
 * @note Single-threaded.
 * @since 0.1.0
 * @brief Verify first error keeps the earliest failure.
 * @details Drives the host model through first error keeps the earliest failure and asserts each observable result.
 * @pre Static fixture storage needed by this scenario is available.
 * @post The focused scenario leaves no unverified result.
 */
RA8_INTERNAL static void internal_test_first_error_keeps_the_earliest_failure(void)
{
  TEST_BEGIN("esp-hosted teardown error precedence");

  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_port_first_error(k_ra8_ok, k_ra8_ok));
  TEST_ASSERT_EQ(k_ra8_err_spi_error,
                 priv_ra8_esp_hosted_port_first_error(k_ra8_err_spi_error, k_ra8_ok));
  TEST_ASSERT_EQ(k_ra8_err_rtos_error,
                 priv_ra8_esp_hosted_port_first_error(k_ra8_ok, k_ra8_err_rtos_error));
  TEST_ASSERT_EQ(k_ra8_err_spi_error,
                 priv_ra8_esp_hosted_port_first_error(k_ra8_err_spi_error, k_ra8_err_rtos_error));

  TEST_END("esp-hosted teardown error precedence");
}

/**
 * @brief Run every esp-hosted port bring-up test in order.
 *
 * @return Process exit status.
 * @retval 0 Every test passed; a failure aborts inside the assertion.
 *
 * @pre The test binary was built with RA8_OFF_TARGET.
 * @pre No other test has brought the port up in this process.
 * @post Every test in this file has run exactly once.
 * @post The port reports not ready.
 *
 * @note Single-threaded.
 * @since 0.1.0
 */
int main(void)
{
  internal_test_cfg_check_rejects_each_field();
  internal_test_pins_check_rejects_duplicates();
  internal_test_pins_check_rejects_out_of_range();
  internal_test_shipped_pin_map_is_consistent();
  internal_test_deinit_refuses_when_never_up();
  internal_test_first_error_keeps_the_earliest_failure();
  return 0;
}
