/**
 * @file test_fw_if_clock_ra8.c
 * @brief Vectors for the RA8 chip adapter behind the neutral clock port.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The table is where every wrong answer in this adapter would live, so it gets
 * exercised exhaustively and the instance-index arithmetic gets pinned against
 * the module-stop ids transcribed from the hardware manual. The ops are then
 * driven through the public port facade rather than called directly, which is
 * the only way to prove the facade and the adapter agree on what each error
 * code means.
 *
 * No module-stop register is written here. `ra8_mstp_enable` polls a hardware
 * bit for read-back and there is no fake peripheral block in this tree to poll
 * against, so the gating vectors stop at the two answers the adapter itself
 * produces: no row, and a row with no gate. The successful-ungate path needs a
 * bench, and this file does not pretend to cover it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fw_if_clock.h"
#include "fw_if_clock_ra8.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_mstp.h"
#include "unity_minimal.h"

/** @brief Module kinds this adapter is expected to resolve at index zero. */
static const fw_clock_module_kind_t k_resolvable[] = {
    k_fw_clock_module_core,     k_fw_clock_module_uart,     k_fw_clock_module_spi,
    k_fw_clock_module_i2c,      k_fw_clock_module_can,      k_fw_clock_module_adc,
    k_fw_clock_module_dac,      k_fw_clock_module_display,  k_fw_clock_module_camera,
    k_fw_clock_module_ethernet, k_fw_clock_module_sdhost,   k_fw_clock_module_crypto,
    k_fw_clock_module_memory,
};

/** @brief Module kinds this adapter deliberately carries no row for. */
static const fw_clock_module_kind_t k_absent[] = {
    k_fw_clock_module_none,  k_fw_clock_module_timer, k_fw_clock_module_pwm,
    k_fw_clock_module_dma,   k_fw_clock_module_usb,   k_fw_clock_module_rtc,
    k_fw_clock_module_watchdog,
};

static void test_resolvable_kinds_resolve_at_index_zero(void)
{
  TEST_BEGIN("every row this adapter claims resolves at index 0");

  for (size_t i = 0U; i < (sizeof(k_resolvable) / sizeof(k_resolvable[0])); ++i) {
    const fw_clock_module_t  module = {.kind = k_resolvable[i], .index = 0U};
    fw_clock_ra8_row_t       row    = {0};
    TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(module, &row));
    TEST_ASSERT(row.has_domain || row.has_gate);
  }

  TEST_END("every row this adapter claims resolves at index 0");
}

static void test_absent_kinds_report_not_found(void)
{
  TEST_BEGIN("kinds with no row report not_found, not a zeroed row");

  for (size_t i = 0U; i < (sizeof(k_absent) / sizeof(k_absent[0])); ++i) {
    const fw_clock_module_t module = {.kind = k_absent[i], .index = 0U};
    fw_clock_ra8_row_t      row    = {0};
    TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_ra8_resolve(module, &row));
    TEST_ASSERT(!row.has_domain);
    TEST_ASSERT(!row.has_gate);
  }

  TEST_END("kinds with no row report not_found, not a zeroed row");
}

static void test_uart_walks_sci0_down_to_sci9(void)
{
  TEST_BEGIN("uart 0..9 walk SCI0 down to SCI9");

  static const ra8_mstp_t k_want[] = {
      k_ra8_mstp_sci0, k_ra8_mstp_sci1, k_ra8_mstp_sci2, k_ra8_mstp_sci3, k_ra8_mstp_sci4,
      k_ra8_mstp_sci5, k_ra8_mstp_sci6, k_ra8_mstp_sci7, k_ra8_mstp_sci8, k_ra8_mstp_sci9,
  };

  for (uint8_t i = 0U; i < (uint8_t)(sizeof(k_want) / sizeof(k_want[0])); ++i) {
    const fw_clock_module_t module = {.kind = k_fw_clock_module_uart, .index = i};
    fw_clock_ra8_row_t      row    = {0};
    TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(module, &row));
    TEST_ASSERT_EQ(k_want[i], row.gate);
    TEST_ASSERT_EQ(k_ra8_clock_id_pclka, row.domain);
  }

  TEST_END("uart 0..9 walk SCI0 down to SCI9");
}

static void test_multi_instance_runs_match_the_manual(void)
{
  TEST_BEGIN("i2c, spi, can, sdhost and dac runs match the MSTP table");

  const fw_clock_module_t i2c2 = {.kind = k_fw_clock_module_i2c, .index = 2U};
  const fw_clock_module_t spi1 = {.kind = k_fw_clock_module_spi, .index = 1U};
  const fw_clock_module_t can1 = {.kind = k_fw_clock_module_can, .index = 1U};
  const fw_clock_module_t sd1  = {.kind = k_fw_clock_module_sdhost, .index = 1U};
  const fw_clock_module_t dac1 = {.kind = k_fw_clock_module_dac, .index = 1U};
  fw_clock_ra8_row_t      row  = {0};

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(i2c2, &row));
  TEST_ASSERT_EQ(k_ra8_mstp_iic2, row.gate);
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(spi1, &row));
  TEST_ASSERT_EQ(k_ra8_mstp_spi1, row.gate);
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(can1, &row));
  TEST_ASSERT_EQ(k_ra8_mstp_canfd1, row.gate);
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(sd1, &row));
  TEST_ASSERT_EQ(k_ra8_mstp_sdhi1, row.gate);
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_resolve(dac1, &row));
  TEST_ASSERT_EQ(k_ra8_mstp_dac12_1, row.gate);

  TEST_END("i2c, spi, can, sdhost and dac runs match the MSTP table");
}

static void test_one_past_the_last_instance_is_not_found(void)
{
  TEST_BEGIN("one past the last instance of a run is not_found");

  const fw_clock_module_t uart10 = {.kind = k_fw_clock_module_uart, .index = 10U};
  const fw_clock_module_t i2c3   = {.kind = k_fw_clock_module_i2c, .index = 3U};
  const fw_clock_module_t core1  = {.kind = k_fw_clock_module_core, .index = 1U};
  fw_clock_ra8_row_t      row    = {0};

  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_ra8_resolve(uart10, &row));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_ra8_resolve(i2c3, &row));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_ra8_resolve(core1, &row));

  TEST_END("one past the last instance of a run is not_found");
}

static void test_resolve_rejects_a_bad_kind_and_a_null_row(void)
{
  TEST_BEGIN("resolve rejects an out-of-range kind and a NULL row");

  const fw_clock_module_t good = {.kind = k_fw_clock_module_uart, .index = 0U};
  const fw_clock_module_t bad  = {.kind = (fw_clock_module_kind_t)K_FW_CLOCK_MODULE_KIND_COUNT,
                                  .index = 0U};
  fw_clock_ra8_row_t      row  = {0};

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_ra8_resolve(good, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_ra8_resolve(bad, &row));

  TEST_END("resolve rejects an out-of-range kind and a NULL row");
}

static void test_bind_helper_produces_a_usable_handle(void)
{
  TEST_BEGIN("the bind helper produces a bound handle");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));
  TEST_ASSERT(clk.bound);
  TEST_ASSERT_EQ(fw_clock_ra8_iface(), clk.iface);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, fw_clock_ra8_bind(nullptr));

  TEST_END("the bind helper produces a bound handle");
}

static void test_rate_reads_back_through_the_facade(void)
{
  TEST_BEGIN("a grounded row reads a nonzero rate through the facade");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));

  const fw_clock_module_t uart0 = {.kind = k_fw_clock_module_uart, .index = 0U};
  const fw_clock_module_t core0 = {.kind = k_fw_clock_module_core, .index = 0U};
  uint32_t                hz    = 0U;

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk, uart0, &hz));
  TEST_ASSERT(hz > 0U);
  hz = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk, core0, &hz));
  TEST_ASSERT(hz > 0U);

  TEST_END("a grounded row reads a nonzero rate through the facade");
}

static void test_gate_only_row_refuses_a_rate(void)
{
  TEST_BEGIN("a gate-only row refuses a rate rather than inventing one");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));

  const fw_clock_module_t display0 = {.kind = k_fw_clock_module_display, .index = 0U};
  uint32_t                hz       = 0U;

  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_rate_for(&clk, display0, &hz));
  TEST_ASSERT_EQ(0U, hz);

  TEST_END("a gate-only row refuses a rate rather than inventing one");
}

static void test_ungateable_row_refuses_to_gate(void)
{
  TEST_BEGIN("a row with no module-stop bit refuses to gate");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));

  const fw_clock_module_t core0   = {.kind = k_fw_clock_module_core, .index = 0U};
  const fw_clock_module_t memory0 = {.kind = k_fw_clock_module_memory, .index = 0U};
  const fw_clock_module_t timer0  = {.kind = k_fw_clock_module_timer, .index = 0U};

  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_enable(&clk, core0));
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_disable(&clk, memory0));
  TEST_ASSERT_EQ(k_ra8_err_not_found, fw_clock_enable(&clk, timer0));

  TEST_END("a row with no module-stop bit refuses to gate");
}

static void test_has_module_separates_resolvable_from_absent(void)
{
  TEST_BEGIN("has_module separates resolvable rows from absent ones");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));

  const fw_clock_module_t uart9 = {.kind = k_fw_clock_module_uart, .index = 9U};
  const fw_clock_module_t usb0  = {.kind = k_fw_clock_module_usb, .index = 0U};
  bool                    present = false;

  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, uart9, &present));
  TEST_ASSERT(present);
  present = true;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, usb0, &present));
  TEST_ASSERT(!present);

  TEST_END("has_module separates resolvable rows from absent ones");
}

static void test_require_compares_against_the_real_rate(void)
{
  TEST_BEGIN("require compares a floor against the rate actually read");

  fw_clock_t clk = {0};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_ra8_bind(&clk));

  const fw_clock_module_t uart0 = {.kind = k_fw_clock_module_uart, .index = 0U};
  uint32_t                hz    = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_rate_for(&clk, uart0, &hz));

  uint32_t got = 0U;
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_require(&clk, uart0, hz, &got));
  TEST_ASSERT_EQ(hz, got);
  got = 0U;
  TEST_ASSERT_EQ(k_ra8_err_not_supported, fw_clock_require(&clk, uart0, hz + 1U, &got));
  TEST_ASSERT_EQ(hz, got);

  TEST_END("require compares a floor against the rate actually read");
}

int main(void)
{
  test_resolvable_kinds_resolve_at_index_zero();
  test_absent_kinds_report_not_found();
  test_uart_walks_sci0_down_to_sci9();
  test_multi_instance_runs_match_the_manual();
  test_one_past_the_last_instance_is_not_found();
  test_resolve_rejects_a_bad_kind_and_a_null_row();
  test_bind_helper_produces_a_usable_handle();
  test_rate_reads_back_through_the_facade();
  test_gate_only_row_refuses_a_rate();
  test_ungateable_row_refuses_to_gate();
  test_has_module_separates_resolvable_from_absent();
  test_require_compares_against_the_real_rate();
  return 0;
}
