/**
 * @file test_ra8_board_ek_ra8d2_clock_profile.c
 * @brief Vectors for the EK-RA8D2 dense clock numbering.
 *
 * @par Tag
 * [Ring 3 / Test] {World: NS}
 *
 * @details
 * The translation is the whole subject. These vectors pin the four wired SCI
 * channels in board order, the single RIIC the board routes, and every kind the
 * board routes nowhere, because a profile that quietly widened would hand an
 * application a module it cannot actually reach.
 *
 * The ops are exercised through ::ra8_board_clock_profile_to_chip rather than
 * through a bound handle: past the translation they are the chip binding, which
 * has its own vectors, and reaching a real CGC register on the host would prove
 * nothing about the numbering.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "fw_if_clock.h"
#include "ra8_board_ek_ra8d2_clock_profile.h"
#include "ra8_err.h"
#include "unity_minimal.h"

/**
 * @brief Translate one module, asserting success, and return the chip index.
 * @param[in] kind  Module kind.
 * @param[in] index Board index.
 * @return Chip instance behind it.
 */
static uint8_t chip_index_of(fw_clock_module_kind_t kind, uint8_t index)
{
  fw_clock_module_t       chip   = {};
  const fw_clock_module_t module = {.kind = kind, .index = index};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_clock_profile_to_chip(module, &chip));
  TEST_ASSERT_EQ((int)kind, (int)chip.kind);
  return chip.index;
}

/**
 * @brief Assert the board refuses @p kind at @p index.
 * @param[in] kind  Module kind.
 * @param[in] index Board index.
 */
static void assert_unwired(fw_clock_module_kind_t kind, uint8_t index)
{
  fw_clock_module_t       chip   = {};
  const fw_clock_module_t module = {.kind = kind, .index = index};
  TEST_ASSERT_EQ(k_ra8_err_not_found, ra8_board_clock_profile_to_chip(module, &chip));
}

/**
 * @brief The four wired SCI channels, in board order.
 * @details This is the vector the whole port exists for: the console is SCI8
 *          and an application asking for it says UART 3.
 */
static void test_uart_is_the_four_wired_channels(void)
{
  TEST_BEGIN("board clock profile: uart is the four wired SCI channels");
  TEST_ASSERT_EQ(4, (int)ra8_board_clock_profile_count(k_fw_clock_module_uart));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_uart, 0U));
  TEST_ASSERT_EQ(2, (int)chip_index_of(k_fw_clock_module_uart, 1U));
  TEST_ASSERT_EQ(7, (int)chip_index_of(k_fw_clock_module_uart, 2U));
  TEST_ASSERT_EQ(8, (int)chip_index_of(k_fw_clock_module_uart, 3U));
  TEST_END("board clock profile: uart is the four wired SCI channels");
}

/**
 * @brief A fifth UART is refused even though the chip has ten SCI channels.
 */
static void test_uart_stops_at_the_wired_count(void)
{
  TEST_BEGIN("board clock profile: uart stops at the wired count");
  assert_unwired(k_fw_clock_module_uart, 4U);
  assert_unwired(k_fw_clock_module_uart, 9U);
  assert_unwired(k_fw_clock_module_uart, 255U);
  TEST_END("board clock profile: uart stops at the wired count");
}

/**
 * @brief The board's one I2C bus is IIC1, presented as I2C 0.
 * @details The renumbering is real here too, and a second row would be the
 *          I3C-backed touch bus, which is a different block.
 */
static void test_i2c_zero_is_chip_one(void)
{
  TEST_BEGIN("board clock profile: i2c 0 is chip IIC1");
  TEST_ASSERT_EQ(1, (int)ra8_board_clock_profile_count(k_fw_clock_module_i2c));
  TEST_ASSERT_EQ(1, (int)chip_index_of(k_fw_clock_module_i2c, 0U));
  assert_unwired(k_fw_clock_module_i2c, 1U);
  TEST_END("board clock profile: i2c 0 is chip IIC1");
}

/**
 * @brief Single-instance blocks pass through unchanged.
 */
static void test_single_instance_kinds_pass_through(void)
{
  TEST_BEGIN("board clock profile: single-instance kinds pass through");
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_core, 0U));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_sdhost, 0U));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_camera, 0U));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_display, 0U));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_ethernet, 0U));
  TEST_ASSERT_EQ(0, (int)chip_index_of(k_fw_clock_module_memory, 0U));
  TEST_END("board clock profile: single-instance kinds pass through");
}

/**
 * @brief Each single-instance kind refuses a second instance.
 */
static void test_single_instance_kinds_refuse_a_second(void)
{
  TEST_BEGIN("board clock profile: single-instance kinds refuse a second");
  assert_unwired(k_fw_clock_module_core, 1U);
  assert_unwired(k_fw_clock_module_sdhost, 1U);
  assert_unwired(k_fw_clock_module_camera, 1U);
  assert_unwired(k_fw_clock_module_display, 1U);
  assert_unwired(k_fw_clock_module_ethernet, 1U);
  assert_unwired(k_fw_clock_module_memory, 1U);
  TEST_END("board clock profile: single-instance kinds refuse a second");
}

/**
 * @brief Kinds this board routes nowhere are refused at instance 0.
 * @details Several of these the chip binding answers for. Going through the
 *          board says only what the board can reach.
 */
static void test_unwired_kinds_are_refused(void)
{
  TEST_BEGIN("board clock profile: unwired kinds are refused");
  assert_unwired(k_fw_clock_module_spi, 0U);
  assert_unwired(k_fw_clock_module_can, 0U);
  assert_unwired(k_fw_clock_module_adc, 0U);
  assert_unwired(k_fw_clock_module_dac, 0U);
  assert_unwired(k_fw_clock_module_usb, 0U);
  assert_unwired(k_fw_clock_module_timer, 0U);
  assert_unwired(k_fw_clock_module_pwm, 0U);
  assert_unwired(k_fw_clock_module_dma, 0U);
  assert_unwired(k_fw_clock_module_rtc, 0U);
  assert_unwired(k_fw_clock_module_watchdog, 0U);
  assert_unwired(k_fw_clock_module_crypto, 0U);
  TEST_END("board clock profile: unwired kinds are refused");
}

/**
 * @brief The count matches what translation accepts, for every kind.
 * @details Walks 0..count-1 and then one past, so a table row whose count
 *          disagrees with its list cannot pass.
 */
static void test_count_agrees_with_translation(void)
{
  TEST_BEGIN("board clock profile: count agrees with translation");
  for (uint32_t k = 0U; k < (uint32_t)K_FW_CLOCK_MODULE_KIND_COUNT; ++k) {
    const fw_clock_module_kind_t kind  = (fw_clock_module_kind_t)k;
    const uint8_t                count = ra8_board_clock_profile_count(kind);
    for (uint8_t i = 0U; i < count; ++i) {
      fw_clock_module_t       chip   = {};
      const fw_clock_module_t module = {.kind = kind, .index = i};
      TEST_ASSERT_EQ(k_ra8_ok, ra8_board_clock_profile_to_chip(module, &chip));
    }
    assert_unwired(kind, count);
  }
  TEST_END("board clock profile: count agrees with translation");
}

/**
 * @brief The none kind and an out-of-range kind are both refused.
 */
static void test_kind_bounds(void)
{
  TEST_BEGIN("board clock profile: kind bounds");
  assert_unwired(k_fw_clock_module_none, 0U);
  TEST_ASSERT_EQ(0, (int)ra8_board_clock_profile_count(k_fw_clock_module_none));
  assert_unwired((fw_clock_module_kind_t)K_FW_CLOCK_MODULE_KIND_COUNT, 0U);
  TEST_ASSERT_EQ(
    0, (int)ra8_board_clock_profile_count((fw_clock_module_kind_t)K_FW_CLOCK_MODULE_KIND_COUNT));
  TEST_END("board clock profile: kind bounds");
}

/**
 * @brief A null output is refused before the table is read.
 */
static void test_null_output_is_refused(void)
{
  TEST_BEGIN("board clock profile: null output is refused");
  const fw_clock_module_t module = {.kind = k_fw_clock_module_uart, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_clock_profile_to_chip(module, nullptr));
  TEST_END("board clock profile: null output is refused");
}

/**
 * @brief Binding refuses a null handle and succeeds on a real one.
 */
static void test_bind(void)
{
  TEST_BEGIN("board clock profile: bind");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_clock_profile_bind(nullptr));
  fw_clock_t clk = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_clock_profile_bind(&clk));
  bool                    present = false;
  const fw_clock_module_t console = {.kind = k_fw_clock_module_uart, .index = 3U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, console, &present));
  TEST_ASSERT(present);
  const fw_clock_module_t can0 = {.kind = k_fw_clock_module_can, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&clk, can0, &present));
  TEST_ASSERT(!present);
  TEST_END("board clock profile: bind");
}

/**
 * @brief The board handle answers the same questions as a hand-bound one.
 */
static void test_board_handle_matches_a_hand_bound_one(void)
{
  TEST_BEGIN("board clock profile: shared handle matches a hand-bound one");
  fw_clock_t mine = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_clock_profile_bind(&mine));

  const fw_clock_t* shared = ra8_board_clock();
  TEST_ASSERT_NOT_NULL(shared);
  TEST_ASSERT(shared->bound);
  TEST_ASSERT_EQ(mine.iface, shared->iface);
  TEST_ASSERT_EQ(mine.ctx, shared->ctx);

  bool                    present   = false;
  bool                    present_2 = false;
  const fw_clock_module_t mikrobus  = {.kind = k_fw_clock_module_uart, .index = 2U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(&mine, mikrobus, &present));
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(shared, mikrobus, &present_2));
  TEST_ASSERT(present);
  TEST_ASSERT_EQ(present, present_2);
  TEST_END("board clock profile: shared handle matches a hand-bound one");
}

/**
 * @brief Repeated acquisition hands back the one handle, already bound.
 */
static void test_board_handle_is_one_handle(void)
{
  TEST_BEGIN("board clock profile: acquisition is idempotent");
  const fw_clock_t* first  = ra8_board_clock();
  const fw_clock_t* second = ra8_board_clock();
  TEST_ASSERT_NOT_NULL(first);
  TEST_ASSERT_EQ(first, second);
  TEST_ASSERT(first->bound);

  /* A port entry point refuses an unbound handle, so a bound answer here is
   * the observable proof that acquisition really did the bind and that the
   * second acquisition did not reset it. */
  bool                    present = false;
  const fw_clock_module_t core    = {.kind = k_fw_clock_module_core, .index = 0U};
  TEST_ASSERT_EQ(k_ra8_ok, fw_clock_has_module(second, core, &present));
  TEST_ASSERT(present);
  TEST_END("board clock profile: acquisition is idempotent");
}

/**
 * @brief Entry point.
 * @return Process exit status.
 */
int main(void)
{
  test_uart_is_the_four_wired_channels();
  test_uart_stops_at_the_wired_count();
  test_i2c_zero_is_chip_one();
  test_single_instance_kinds_pass_through();
  test_single_instance_kinds_refuse_a_second();
  test_unwired_kinds_are_refused();
  test_count_agrees_with_translation();
  test_kind_bounds();
  test_null_output_is_refused();
  test_bind();
  test_board_handle_matches_a_hand_bound_one();
  test_board_handle_is_one_handle();
  return 0;
}
