/**
 * @file test_ra8_esp_hosted_gpio.c
 * @brief Unit tests for the esp-hosted side-band GPIO slots and edge detector
 *
 * @details
 * Host-only tests for ``port/esp-hosted/src/ra8_esp_hosted_gpio.c`` and its
 * companion ``ra8_esp_hosted_gpio_edge.c``. Every level read and write is
 * routed through the slice's own ``ra8_pin_interface_t`` seam, so the tests
 * drive pin levels directly instead of reaching for hardware; pin
 * configuration, claiming and interrupt attachment still run for real against
 * the fake's RAM-backed PORT, PFS and ICU blocks.
 *
 * What we cover:
 *   - the port/pin encode-decode round trip, including port 0 (which encodes
 *     as a null pointer) and the ``-1`` unwired sentinel;
 *   - every rejection path of all eight slots;
 *   - the software edge detector's decision across all four senses and all
 *     four (previous, current) level pairs;
 *   - registration until the polled table is full, and teardown of both the
 *     hardware-interrupt and polled paths;
 *   - the honest refusals: a pull-down request and a pin-hold request.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "esp_hosted_os_abstraction.h"
#include "esp_hosted_power_save.h"
#include "port_esp_hosted_host_config.h"
#include "port_esp_hosted_host_os.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_esp_hosted_gpio_internal.h"
#include "ra8_esp_hosted_pins.h"
#include "ra8_fake_mmap.h"
#include "ra8_icu.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_pin_interface.h"
#include "ra8_pin_validator.h"
#include "ra8_port_constants.h"
#include "ra8_reset.h"
#include "ra8_reset_regs.h"
#include "unity_minimal.h"

/**
 * @enum gpio_test_const_t
 * @brief Fixtures and bounds the GPIO test cases need.
 */
typedef enum : uint16_t {
  k_gpio_test_level_rows = 8U,  /**< Rows in the mock level table.      */
  k_gpio_test_bad_port   = 15U, /**< One past ``k_ra8_port_max``.       */
  k_gpio_test_bad_pin    = 16U, /**< One past ``k_ra8_pin_max``.        */
  k_gpio_test_port_zero  = 0U,  /**< Port 0: encodes as a null pointer. */
  k_gpio_test_pin_three  = 3U,  /**< An arbitrary in-range pin index.   */
  k_gpio_test_port_six   = 6U,  /**< An arbitrary non-zero port index.  */
  k_gpio_test_pin_eleven = 11U, /**< An arbitrary in-range pin index.   */
  k_gpio_test_bad_sense  = 4U,  /**< One past ``k_ra8_icu_irqmd_low``.  */
  k_gpio_test_bad_mode   = 7U,  /**< Neither input nor output.          */
  k_gpio_test_bad_pull   = 9U,  /**< Neither pull-up nor pull-down.     */
  k_gpio_test_poll_ms    = 5U,  /**< A legal sampling period.           */
  k_gpio_test_level_low  = 0U,  /**< The low level the mock stores.     */
  k_gpio_test_level_high = 1U,  /**< The high level the mock stores.    */
} gpio_test_const_t;

/**
 * @enum gpio_test_wide_const_t
 * @brief Fixtures that do not fit the 16-bit constant enum above.
 */
typedef enum : uint32_t {
  /** The vendored ``-1`` unwired sentinel as it arrives through a slot. */
  k_gpio_test_unwired = 0xFFFFFFFFU,
} gpio_test_wide_const_t;

/**
 * @struct mock_level_t
 * @brief One remembered pin level in the recording pin driver.
 */
typedef struct {
  ra8_port_pin_t pin;   /**< Packed pin the row describes.   */
  uint8_t        level; /**< Level the mock reports for it.  */
  bool           used;  /**< True while the row is occupied. */
} mock_level_t;

/** @brief Levels the recording pin driver reports and remembers. */
static mock_level_t s_levels[k_gpio_test_level_rows];
/** @brief Set to make every mock read fail, reaching the read-failure paths. */
static bool s_read_fails;
/** @brief Number of edge callbacks the detector has dispatched. */
static uint32_t s_handler_calls;
/** @brief Argument of the most recent edge callback. */
static void* s_handler_arg;

/**
 * @brief Find or create the mock level row for a pin.
 * @details Reuses a matching row before claiming the first bounded free slot.
 * @param[in] pin Logical port/pin identifier to find or insert.
 * @return Fixture row for @p pin, or nullptr when the table is full.
 * @retval nullptr All fixture rows are already occupied by other pins.
 * @pre Host mock storage is initialized. @pre @p pin uses the packed RA8 format.
 * @post A returned row is marked used. @post Existing rows keep their levels.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static mock_level_t* internal_mock_row(ra8_port_pin_t pin)
{
  for (uint16_t i = 0U; i < (uint16_t)k_gpio_test_level_rows; i++) {
    if (s_levels[i].used && (s_levels[i].pin == pin)) {
      return &s_levels[i];
    }
  }
  for (uint16_t i = 0U; i < (uint16_t)k_gpio_test_level_rows; i++) {
    if (!s_levels[i].used) {
      s_levels[i].used = true;
      s_levels[i].pin  = pin;
      return &s_levels[i];
    }
  }
  return nullptr;
}

/** @brief Recording ``output_init`` row of the mock pin driver.
 * @details Implements the fixture-only mock output init operation with bounded static state.
 * @param[in,out] ctx Backend context supplied by the adapter under test.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @param[in] init_level Initial logical output level requested by the adapter.
 * @return Mock status returned to the adapter under test.
 * @retval k_ra8_ok The deterministic mock operation completed.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mock_output_init(void* ctx, ra8_port_pin_t pin, ra8_level_t init_level)
{
  (void)ctx;
  mock_level_t* row = internal_mock_row(pin);
  if (row == nullptr) {
    return k_ra8_err_no_mem;
  }
  row->level = (uint8_t)init_level;
  return k_ra8_ok;
}

/** @brief Recording ``write`` row of the mock pin driver.
 * @details Implements the fixture-only mock write operation with bounded static state.
 * @param[in,out] ctx Backend context supplied by the adapter under test.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @param[in] level Logical level to record for the selected pin.
 * @return Mock status returned to the adapter under test.
 * @retval k_ra8_ok The deterministic mock operation completed.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mock_write(void* ctx, ra8_port_pin_t pin, ra8_level_t level)
{
  return internal_mock_output_init(ctx, pin, level);
}

/** @brief Recording ``read`` row of the mock pin driver.
 * @details Implements the fixture-only mock read operation with bounded static state.
 * @param[in,out] ctx Backend context supplied by the adapter under test.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @param[out] out_level Destination that receives the recorded logical level.
 * @return Mock status returned to the adapter under test.
 * @retval k_ra8_ok The deterministic mock operation completed.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t
internal_mock_read(void* ctx, ra8_port_pin_t pin, ra8_level_t* out_level)
{
  (void)ctx;
  if (s_read_fails || (out_level == nullptr)) {
    return k_ra8_err_hw_timeout;
  }
  const mock_level_t* row = internal_mock_row(pin);
  if (row == nullptr) {
    return k_ra8_err_no_mem;
  }
  *out_level = (row->level != 0U) ? k_ra8_level_high : k_ra8_level_low;
  return k_ra8_ok;
}

/** @brief Recording ``toggle`` row of the mock pin driver.
 * @details Implements the fixture-only mock toggle operation with bounded static state.
 * @param[in,out] ctx Backend context supplied by the adapter under test.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @return Mock status returned to the adapter under test.
 * @retval k_ra8_ok The deterministic mock operation completed.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static ra8_err_t internal_mock_toggle(void* ctx, ra8_port_pin_t pin)
{
  (void)ctx;
  mock_level_t* row = internal_mock_row(pin);
  if (row == nullptr) {
    return k_ra8_err_no_mem;
  }
  row->level = (row->level != (uint8_t)k_gpio_test_level_low) ? (uint8_t)k_gpio_test_level_low
                                                              : (uint8_t)k_gpio_test_level_high;
  return k_ra8_ok;
}

/** @brief The recording pin driver injected in place of the HAL's. */
static const ra8_pin_interface_t s_mock_pin_if = {
  .output_init = internal_mock_output_init,
  .write       = internal_mock_write,
  .read        = internal_mock_read,
  .toggle      = internal_mock_toggle,
  .ctx         = nullptr,
};

/** @brief Edge callback the detector and the ICU trampoline both dispatch to.
 * @details Implements the fixture-only count handler operation with bounded static state.
 * @param[in,out] arg Callback-owned fixture state passed through the host model.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_count_handler(void* arg)
{
  s_handler_calls++;
  s_handler_arg = arg;
}

/** @brief Drive the mock's remembered level for a pin.
 * @details Implements the fixture-only set level operation with bounded static state.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @param[in] level Logical level to record for the selected pin.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_set_level(ra8_port_pin_t pin, uint8_t level)
{
  mock_level_t* row = internal_mock_row(pin);
  TEST_ASSERT_NOT_NULL((void*)row);
  row->level = level;
}

/** @brief Read the mock's remembered level for a pin.
 * @details Implements the fixture-only get level operation with bounded static state.
 * @param[in] pin Logical port/pin identifier presented to the mock.
 * @return Recorded low/high byte for the selected pin.
 * @retval 0 The remembered pin is low.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static uint8_t internal_get_level(ra8_port_pin_t pin)
{
  const mock_level_t* row = internal_mock_row(pin);
  TEST_ASSERT_NOT_NULL((void*)row);
  return row->level;
}

/** @brief Return the vtable with the eight GPIO rows bound.
 * @details Implements the fixture-only bound vtable operation with bounded static state.
 * @return Fully bound host vtable used by the focused test.
 * @retval initialized All adapter rows were bound successfully.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static hosted_osi_funcs_t internal_bound_vtable(void)
{
  hosted_osi_funcs_t funcs = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_gpio_bind(&funcs));
  return funcs;
}

/** @brief Restore every module and fake fixture to a known state.
 * @details Implements the fixture-only reset state operation with bounded static state.
 * @pre Host mock storage is initialized. @pre Pointer arguments follow their directions.
 * @post The mock transition is observable. @post No physical hardware is accessed.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_reset_state(void)
{
  ra8_fake_mmap_reset();
  ra8_pin_validator_reset();
  (void)ra8_icu_init();
  (void)ra8_isr_init();
  ra8_reset_test_only_reset_state();
  for (uint16_t i = 0U; i < (uint16_t)k_gpio_test_level_rows; i++) {
    s_levels[i] = (mock_level_t){};
  }
  s_read_fails    = false;
  s_handler_calls = 0U;
  s_handler_arg   = nullptr;
  priv_ra8_esp_hosted_gpio_set_pin_interface(&s_mock_pin_if);
}

/**
 * @par MC/DC:
 * Decision: `(gpio_num == unwired) || (port_idx > port_max) || (gpio_num >
 * pin_max)` in
 * `port/esp-hosted/src/ra8_esp_hosted_gpio.c@priv_ra8_esp_hosted_gpio_decode_pin`
 * (3 conditions).
 * - Vector 1: (F,F,F) port 0 pin 3        -> accepted (the control vector)
 * - Vector 2: (T,F,F) pin 0xFFFFFFFF      -> rejected (varies condition 1)
 * - Vector 3: (F,T,F) port 15, pin 3      -> rejected (varies condition 2)
 * - Vector 4: (F,F,T) port 0, pin 16      -> rejected (varies condition 3)
 * Pairing vector 1 with each of 2, 3 and 4 proves that condition's
 * independent influence. N+1 = 4 vectors for N=3 conditions: minimal MC/DC.
 * The null-`out_pin` guard is a separate single-condition decision and is
 * covered by the two vectors at the end of this case.
 * @brief Verify decode pin.
 * @details Drives the host model through decode pin and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_decode_pin(void)
{
  TEST_BEGIN("esp_hosted gpio: port/pin decode round trip and rejections");
  internal_reset_state();
  ra8_port_pin_t pin = k_ra8_pin_none;

  /* Vector 1: port 0 encodes as a NULL void*, which is legal, not "absent". */
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_decode_pin((void*)(uintptr_t)k_gpio_test_port_zero,
                                                  (uint32_t)k_gpio_test_pin_three,
                                                  &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_gpio_test_port_zero, k_gpio_test_pin_three), pin);

  /* A non-zero port round-trips through the production encoding macros. */
  const ra8_port_pin_t src = RA8_PIN(k_gpio_test_port_six, k_gpio_test_pin_eleven);
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_decode_pin(RA8_ESP_HOSTED_GPIO_PORT(src),
                                                  (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(src),
                                                  &pin));
  TEST_ASSERT_EQ(src, pin);

  /* Vector 2: the vendored "-1" sentinel, widened through uint32_t. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_decode_pin((void*)(uintptr_t)k_gpio_test_port_zero,
                                                   (uint32_t)k_gpio_test_unwired,
                                                   &pin));
  /* Vector 3: port index past the last RA8 port. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_decode_pin((void*)(uintptr_t)k_gpio_test_bad_port,
                                                   (uint32_t)k_gpio_test_pin_three,
                                                   &pin));
  /* Vector 4: pin index past the last pin of a port. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_decode_pin((void*)(uintptr_t)k_gpio_test_port_zero,
                                                   (uint32_t)k_gpio_test_bad_pin,
                                                   &pin));
  /* The null-output guard, both ways. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_decode_pin((void*)(uintptr_t)k_gpio_test_port_zero,
                                                   (uint32_t)k_gpio_test_pin_three,
                                                   nullptr));
  TEST_END("esp_hosted gpio: port/pin decode round trip and rejections");
}

/**
 * @par MC/DC:
 * (no compound decision in the code under test -- this case asserts that the
 * binder populates exactly the eight GPIO rows and rejects a null vtable)
 * @brief Verify bind.
 * @details Drives the host model through bind and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_bind(void)
{
  TEST_BEGIN("esp_hosted gpio: bind populates the eight GPIO rows");
  internal_reset_state();
  TEST_ASSERT_EQ(k_ra8_err_null_ptr, priv_ra8_esp_hosted_gpio_bind(nullptr));

  hosted_osi_funcs_t funcs = {};
  TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_gpio_bind(&funcs));
  TEST_ASSERT_NOT_NULL((void*)funcs._h_config_gpio);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_config_gpio_as_interrupt);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_teardown_gpio_interrupt);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_read_gpio);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_write_gpio);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_pull_gpio);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_hold_gpio);
  TEST_ASSERT_NOT_NULL((void*)funcs._h_get_host_wakeup_or_reboot_reason);
  /* Rows this slice does not own must be left for the other slices. */
  TEST_ASSERT_NULL((void*)funcs._h_do_bus_transfer);
  TEST_END("esp_hosted gpio: bind populates the eight GPIO rows");
}

/**
 * @par MC/DC:
 * (no compound decision -- the mode dispatch is an if / else-if / else chain
 * of single-condition tests, and this case takes all three arms plus the
 * decode rejection)
 * @brief Verify config gpio.
 * @details Drives the host model through config gpio and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_config_gpio(void)
{
  TEST_BEGIN("esp_hosted gpio: config_gpio directions and refusals");
  internal_reset_state();
  const hosted_osi_funcs_t f   = internal_bound_vtable();
  const ra8_port_pin_t     pin = RA8_PIN(k_gpio_test_port_six, k_gpio_test_pin_eleven);
  void* const              prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t           num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  /* An output comes up at the level that leaves the co-processor running. */
  TEST_ASSERT_EQ(RET_OK, f._h_config_gpio(prt, num, (uint32_t)H_GPIO_MODE_DEF_OUTPUT));
  TEST_ASSERT_EQ(H_RESET_VAL_INACTIVE, internal_get_level(pin));

  /* Input configuration goes straight to the HAL: it has no seam row. */
  TEST_ASSERT_EQ(RET_OK, f._h_config_gpio(prt, num, (uint32_t)H_GPIO_MODE_DEF_INPUT));
  TEST_ASSERT(ra8_pin_validator_is_claimed(pin));

  TEST_ASSERT_EQ(RET_INVALID, f._h_config_gpio(prt, num, (uint32_t)k_gpio_test_bad_mode));
  TEST_ASSERT_EQ(
    RET_INVALID,
    f._h_config_gpio(prt, (uint32_t)k_gpio_test_unwired, (uint32_t)H_GPIO_MODE_DEF_INPUT));
  TEST_END("esp_hosted gpio: config_gpio directions and refusals");
}

/**
 * @par MC/DC:
 * Decision: `if (s_read_fails || (out_level == nullptr))` lives in the mock;
 * the code under test carries only single-condition tests. This case takes
 * both arms of the slot's read-failure test (read succeeds / read fails) and
 * both arms of its level test (pin low / pin high), plus the decode
 * rejection on each of the two slots.
 * @brief Verify read write.
 * @details Drives the host model through read write and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_read_write(void)
{
  TEST_BEGIN("esp_hosted gpio: read returns the raw level, write drives it");
  internal_reset_state();
  const hosted_osi_funcs_t f   = internal_bound_vtable();
  const ra8_port_pin_t     pin = RA8_PIN(k_gpio_test_port_six, k_gpio_test_pin_eleven);
  void* const              prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t           num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  internal_set_level(pin, (uint8_t)H_DR_VAL_INACTIVE);
  TEST_ASSERT_EQ(H_DR_VAL_INACTIVE, f._h_read_gpio(prt, num));
  internal_set_level(pin, (uint8_t)H_DR_VAL_ACTIVE);
  TEST_ASSERT_EQ(H_DR_VAL_ACTIVE, f._h_read_gpio(prt, num));

  /* Both failure codes are negative, so neither can be read as a level. */
  TEST_ASSERT(f._h_read_gpio(prt, (uint32_t)k_gpio_test_unwired) < 0);
  s_read_fails = true;
  TEST_ASSERT(f._h_read_gpio(prt, num) < 0);
  s_read_fails = false;

  TEST_ASSERT_EQ(RET_OK, f._h_write_gpio(prt, num, (uint32_t)H_RESET_VAL_ACTIVE));
  TEST_ASSERT_EQ(H_RESET_VAL_ACTIVE, internal_get_level(pin));
  TEST_ASSERT_EQ(RET_OK, f._h_write_gpio(prt, num, (uint32_t)H_RESET_VAL_INACTIVE));
  TEST_ASSERT_EQ(H_RESET_VAL_INACTIVE, internal_get_level(pin));
  TEST_ASSERT_EQ(
    RET_INVALID,
    f._h_write_gpio(prt, (uint32_t)k_gpio_test_unwired, (uint32_t)H_RESET_VAL_INACTIVE));
  TEST_END("esp_hosted gpio: read returns the raw level, write drives it");
}

/**
 * @par MC/DC:
 * (no compound decision -- the pull dispatch is a chain of single-condition
 * tests, and this case takes the pull-down refusal, the unknown-selector
 * refusal, both arms of the enable test, and the decode rejection)
 * @brief Verify pull.
 * @details Drives the host model through pull and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pull(void)
{
  TEST_BEGIN("esp_hosted gpio: pull-up is real, pull-down is refused");
  internal_reset_state();
  const hosted_osi_funcs_t f   = internal_bound_vtable();
  const ra8_port_pin_t     pin = RA8_PIN(k_gpio_test_port_six, k_gpio_test_pin_eleven);
  void* const              prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t           num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  /* The RA8D2 PFS has no pull-down bit; the request must not report success. */
  TEST_ASSERT(f._h_pull_gpio(prt, num, (uint32_t)H_GPIO_PULL_DOWN, (uint32_t)H_ENABLE) != RET_OK);
  TEST_ASSERT(!ra8_pin_validator_is_claimed(pin));

  TEST_ASSERT_EQ(RET_INVALID,
                 f._h_pull_gpio(prt, num, (uint32_t)k_gpio_test_bad_pull, (uint32_t)H_ENABLE));
  TEST_ASSERT_EQ(RET_OK, f._h_pull_gpio(prt, num, (uint32_t)H_GPIO_PULL_UP, (uint32_t)H_ENABLE));
  TEST_ASSERT(ra8_pin_validator_is_claimed(pin));
  TEST_ASSERT_EQ(RET_OK, f._h_pull_gpio(prt, num, (uint32_t)H_GPIO_PULL_UP, (uint32_t)H_DISABLE));
  TEST_ASSERT_EQ(RET_INVALID,
                 f._h_pull_gpio(prt,
                                (uint32_t)k_gpio_test_unwired,
                                (uint32_t)H_GPIO_PULL_UP,
                                (uint32_t)H_ENABLE));
  TEST_END("esp_hosted gpio: pull-up is real, pull-down is refused");
}

/**
 * @par MC/DC:
 * (no compound decision -- the hold slot has one single-condition decode
 * guard, and this case takes both of its arms)
 * @brief Verify hold and wakeup reason.
 * @details Drives the host model through hold and wakeup reason and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hold_and_wakeup_reason(void)
{
  TEST_BEGIN("esp_hosted gpio: hold refuses honestly, wakeup reason is real");
  internal_reset_state();
  const hosted_osi_funcs_t f   = internal_bound_vtable();
  const ra8_port_pin_t     pin = RA8_PIN(k_gpio_test_port_six, k_gpio_test_pin_eleven);
  void* const              prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t           num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  /* No per-pin retention control exists, so the slot must never claim one. */
  TEST_ASSERT(f._h_hold_gpio(prt, num, (uint32_t)H_ENABLE) != RET_OK);
  TEST_ASSERT_EQ(RET_INVALID,
                 f._h_hold_gpio(prt, (uint32_t)k_gpio_test_unwired, (uint32_t)H_DISABLE));

  /* With no flag latched the answer is an ordinary reboot. */
  TEST_ASSERT_EQ(HOSTED_WAKEUP_NORMAL_REBOOT, f._h_get_host_wakeup_or_reboot_reason());

  /* HUM Ch 6.2.2 "RSTSR0" p 257-258 */
  *ra8_reset_rstsr0() = (uint8_t)k_ra8_reset_rstsr0_dpsrstf_msk;
  ra8_reset_test_only_reset_state();
  TEST_ASSERT_EQ(HOSTED_WAKEUP_DEEP_SLEEP, f._h_get_host_wakeup_or_reboot_reason());
  TEST_END("esp_hosted gpio: hold refuses honestly, wakeup reason is real");
}

/**
 * @par MC/DC:
 * `priv_ra8_esp_hosted_gpio_edge_seen` is a chain of five single-condition
 * decisions, so each needs two vectors:
 * - range guard: sense=4 (out of range, false) vs sense=3 (in range);
 * - level-sense test: sense=3 (taken) vs sense=2 (not taken);
 * - change test: prev==now (false) vs prev!=now, at senses 0, 1 and 2;
 * - both-edge test: sense=2 (true on any change) vs sense=1 (not taken);
 * - rising test: sense=1 (true only on 0->1) vs sense=0 (true only on 1->0).
 * The sweep below drives all four senses across all four (prev, now) pairs,
 * which contains both vectors of every one of those decisions.
 * @brief Verify edge seen matrix.
 * @details Drives the host model through edge seen matrix and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_edge_seen_matrix(void)
{
  TEST_BEGIN("esp_hosted gpio: edge detection across every sense and pair");
  internal_reset_state();

  /* Falling: only a 1 -> 0 transition counts. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 0U, (uint8_t)k_ra8_icu_irqmd_falling));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, (uint8_t)k_ra8_icu_irqmd_falling));
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(1U, 0U, (uint8_t)k_ra8_icu_irqmd_falling));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(1U, 1U, (uint8_t)k_ra8_icu_irqmd_falling));

  /* Rising: only a 0 -> 1 transition counts. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 0U, (uint8_t)k_ra8_icu_irqmd_rising));
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, (uint8_t)k_ra8_icu_irqmd_rising));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(1U, 0U, (uint8_t)k_ra8_icu_irqmd_rising));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(1U, 1U, (uint8_t)k_ra8_icu_irqmd_rising));

  /* Both: any transition counts, no transition does not. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 0U, (uint8_t)k_ra8_icu_irqmd_both));
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, (uint8_t)k_ra8_icu_irqmd_both));
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(1U, 0U, (uint8_t)k_ra8_icu_irqmd_both));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(1U, 1U, (uint8_t)k_ra8_icu_irqmd_both));

  /* Low level: the current sample alone decides, exactly as the ICU does. */
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(0U, 0U, (uint8_t)k_ra8_icu_irqmd_low));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, (uint8_t)k_ra8_icu_irqmd_low));
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_edge_seen(1U, 0U, (uint8_t)k_ra8_icu_irqmd_low));
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(1U, 1U, (uint8_t)k_ra8_icu_irqmd_low));

  /* An unknown sense reports no event rather than guessing one. */
  TEST_ASSERT(!priv_ra8_esp_hosted_gpio_edge_seen(0U, 1U, (uint8_t)k_gpio_test_bad_sense));
  TEST_END("esp_hosted gpio: edge detection across every sense and pair");
}

/**
 * @par MC/DC:
 * The slot's own `irq_num == none` selector is single-condition -- this case
 * takes its hardware arm, which the DATA_READY net satisfies because the
 * package routes it to an ICU channel, and then tears it down again.
 * It also drives the row scan
 * `if (s_irq_rows[i].used && (s_irq_rows[i].pin == pin))` in
 * `port/esp-hosted/src/ra8_esp_hosted_gpio.c@internal_irq_find`
 * (2 conditions), which both the registration's duplicate check and the
 * teardown's lookup run, and it installs -- but cannot fire -- the guard in
 * `port/esp-hosted/src/ra8_esp_hosted_gpio.c@internal_isr_trampoline`.
 * The row-scan vectors are:
 * - Vector 1: used=T, pin matches -> true; the second registration of
 *   DATA_READY is refused and the teardown finds its row (the control vector)
 * - Vector 2: used=F              -> false; every row is free on the first
 *   registration and again after teardown (varies `used`)
 * - Vector 3: used=T, pin differs -> false; the HANDSHAKE teardown below runs
 *   while DATA_READY holds a row, so the scan crosses an occupied,
 *   non-matching row and falls through to the polled table (varies the pin
 *   comparison)
 * Vectors 1+2 prove `used` independently affects the outcome; 1+3 prove the
 * same for the pin comparison. N+1 = 3 vectors for N=2: minimal MC/DC.
 *
 * The trampoline has no duplicate pointer decision: registration rejects a
 * null handler and publishes the completed row before enabling the ICU, which
 * is the invariant its interrupt-context callback consumes.
 * @brief Verify hardware interrupt path.
 * @details Drives the host model through hardware interrupt path and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_hardware_interrupt_path(void)
{
  TEST_BEGIN("esp_hosted gpio: a pin with an ICU channel takes the hardware path");
  internal_reset_state();
  const hosted_osi_funcs_t f = internal_bound_vtable();
  /* HANDSHAKE is the net the package routes to an ICU channel (IRQ11 on
     P006); DATA_READY lands on P402, which has none, so it is the spare that
     must fall to the polled table. */
  const ra8_port_pin_t pin       = (ra8_port_pin_t)k_ra8_esp_hosted_pin_handshake;
  void* const          prt       = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t       num       = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);
  const ra8_port_pin_t spare     = (ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready;
  void* const          spare_prt = RA8_ESP_HOSTED_GPIO_PORT(spare);
  const uint32_t       spare_num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(spare);

  TEST_ASSERT(ra8_esp_hosted_pin_irq_num(pin) != (uint8_t)k_ra8_esp_hosted_irq_none);
  TEST_ASSERT_EQ(RET_OK,
                 f._h_config_gpio_as_interrupt(prt,
                                               num,
                                               (uint32_t)H_DR_INTR_EDGE,
                                               internal_count_handler,
                                               &s_levels[0]));
  /* The hardware path must not consume a row of the polled table. */
  TEST_ASSERT_EQ(0U, priv_ra8_esp_hosted_gpio_edge_count());
  TEST_ASSERT(ra8_pin_validator_is_claimed(pin));

  /* A second registration of the same pin is refused, not silently replaced. */
  TEST_ASSERT_EQ(RET_FAIL,
                 f._h_config_gpio_as_interrupt(prt,
                                               num,
                                               (uint32_t)H_DR_INTR_EDGE,
                                               internal_count_handler,
                                               nullptr));

  /* Row 0 is occupied by DATA_READY, so looking up a different pin crosses an
     occupied, non-matching row: the scan must keep walking rather than answer
     from the first entry, then fall through to the polled table, which does
     not hold this pin either. */
  TEST_ASSERT_EQ(RET_FAIL, f._h_teardown_gpio_interrupt(spare_prt, spare_num));

  TEST_ASSERT_EQ(RET_OK, f._h_teardown_gpio_interrupt(prt, num));
  TEST_ASSERT(!ra8_pin_validator_is_claimed(pin));
  /* Tearing down a pin neither path holds is reported, not swallowed. */
  TEST_ASSERT_EQ(RET_FAIL, f._h_teardown_gpio_interrupt(prt, num));
  TEST_END("esp_hosted gpio: a pin with an ICU channel takes the hardware path");
}

/**
 * @par MC/DC:
 * (no compound decision -- this case takes each single-condition rejection
 * guard of the interrupt-configuration slot: bad pin pair, null handler and
 * out-of-range sense)
 * @brief Verify interrupt rejections.
 * @details Drives the host model through interrupt rejections and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_interrupt_rejections(void)
{
  TEST_BEGIN("esp_hosted gpio: interrupt configuration rejects bad arguments");
  internal_reset_state();
  const hosted_osi_funcs_t f   = internal_bound_vtable();
  const ra8_port_pin_t     pin = (ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready;
  void* const              prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t           num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  TEST_ASSERT_EQ(RET_INVALID,
                 f._h_config_gpio_as_interrupt(prt,
                                               (uint32_t)k_gpio_test_unwired,
                                               (uint32_t)H_DR_INTR_EDGE,
                                               internal_count_handler,
                                               nullptr));
  TEST_ASSERT_EQ(
    RET_INVALID,
    f._h_config_gpio_as_interrupt(prt, num, (uint32_t)H_DR_INTR_EDGE, nullptr, nullptr));
  TEST_ASSERT_EQ(RET_INVALID,
                 f._h_config_gpio_as_interrupt(prt,
                                               num,
                                               (uint32_t)k_gpio_test_bad_sense,
                                               internal_count_handler,
                                               nullptr));
  TEST_ASSERT_EQ(RET_INVALID, f._h_teardown_gpio_interrupt(prt, (uint32_t)k_gpio_test_unwired));
  TEST_ASSERT_EQ(0U, priv_ra8_esp_hosted_gpio_edge_count());
  TEST_END("esp_hosted gpio: interrupt configuration rejects bad arguments");
}

/**
 * @par MC/DC:
 * Decision: `if (row->used && read_ok)` in
 * `port/esp-hosted/src/ra8_esp_hosted_gpio_edge.c@priv_ra8_esp_hosted_gpio_edge_poll_once`
 * (2 conditions).
 * - Vector 1: used=T, read_ok=T -> the handler runs (the control vector)
 * - Vector 2: used=F, read_ok=- -> no handler; the three free rows of the
 *   table are walked on every pass and must dispatch nothing
 * - Vector 3: used=T, read_ok=F -> no handler; the mock read is armed to fail
 * Vectors 1+2 prove `used` influences the outcome independently, 1+3 prove
 * the same for the read result. N+1 = 3 vectors for N=2 conditions.
 * @brief Verify polled path.
 * @details Drives the host model through polled path and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_polled_path(void)
{
  TEST_BEGIN("esp_hosted gpio: a pin with no ICU channel is sampled in software");
  internal_reset_state();
  const hosted_osi_funcs_t f = internal_bound_vtable();
  /* DATA_READY lands on P402, which the package routes to no ICU channel, so
     it is the net that must fall to the software edge detector. */
  const ra8_port_pin_t pin = (ra8_port_pin_t)k_ra8_esp_hosted_pin_data_ready;
  void* const          prt = RA8_ESP_HOSTED_GPIO_PORT(pin);
  const uint32_t       num = (uint32_t)RA8_ESP_HOSTED_GPIO_PIN(pin);

  TEST_ASSERT_EQ(k_ra8_esp_hosted_irq_none, ra8_esp_hosted_pin_irq_num(pin));
  internal_set_level(pin, (uint8_t)H_DR_VAL_INACTIVE);
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_ra8_esp_hosted_gpio_set_edge_poll_ms((uint16_t)k_gpio_test_poll_ms));
  TEST_ASSERT_EQ(RET_OK,
                 f._h_config_gpio_as_interrupt(prt,
                                               num,
                                               (uint32_t)H_DR_INTR_EDGE,
                                               internal_count_handler,
                                               &s_levels[1]));
  TEST_ASSERT_EQ(1U, priv_ra8_esp_hosted_gpio_edge_count());

  /* Vector 2: nothing changed and three rows are free -- no dispatch. */
  priv_ra8_esp_hosted_gpio_edge_poll_once();
  TEST_ASSERT_EQ(0U, s_handler_calls);

  /* Vector 1: the asserting edge arrives and the handler runs exactly once. */
  internal_set_level(pin, (uint8_t)H_HS_VAL_ACTIVE);
  priv_ra8_esp_hosted_gpio_edge_poll_once();
  TEST_ASSERT_EQ(1U, s_handler_calls);
  TEST_ASSERT(s_handler_arg == &s_levels[1]);
  priv_ra8_esp_hosted_gpio_edge_poll_once();
  TEST_ASSERT_EQ(1U, s_handler_calls);

  /* Vector 3: a failed read must not manufacture an edge. */
  internal_set_level(pin, (uint8_t)H_HS_VAL_INACTIVE);
  s_read_fails = true;
  priv_ra8_esp_hosted_gpio_edge_poll_once();
  TEST_ASSERT_EQ(1U, s_handler_calls);
  s_read_fails = false;

  TEST_ASSERT_EQ(RET_OK, f._h_teardown_gpio_interrupt(prt, num));
  TEST_ASSERT_EQ(0U, priv_ra8_esp_hosted_gpio_edge_count());
  TEST_END("esp_hosted gpio: a pin with no ICU channel is sampled in software");
}

/**
 * @par MC/DC:
 * The registration's own guards are single-condition -- this case takes the
 * table-full arm of the capacity test, plus the null-handler, bad-sense and
 * duplicate guards, and both arms of the period setter. The one compound
 * decision it drives is the row scan
 * `if (s_rows[i].used && (s_rows[i].pin == pin))` in
 * `port/esp-hosted/src/ra8_esp_hosted_gpio_edge.c@internal_find`
 * (2 conditions), which the duplicate check runs on every registration:
 * - Vector 1: used=T, pin matches   -> true; the duplicate is refused
 *   (the control vector, from re-registering each pin)
 * - Vector 2: used=F                -> false; a free row is skipped, which
 *   is how the first registration finds no duplicate (varies `used`)
 * - Vector 3: used=T, pin differs   -> false; registering the second and
 *   later pins walks the rows the earlier ones took (varies the pin
 *   comparison)
 * Vectors 1+2 prove `used` independently affects the outcome; 1+3 prove the
 * same for the pin comparison. N+1 = 3 vectors for N=2: minimal MC/DC.
 * @brief Verify polled table bounds.
 * @details Drives the host model through polled table bounds and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_polled_table_bounds(void)
{
  TEST_BEGIN("esp_hosted gpio: the polled table fills up and then refuses");
  internal_reset_state();
  const uint8_t sense = (uint8_t)k_ra8_icu_irqmd_rising;

  TEST_ASSERT_EQ(k_ra8_err_null_ptr,
                 priv_ra8_esp_hosted_gpio_edge_register(RA8_PIN(k_ra8_port_4, k_ra8_pin_0),
                                                        sense,
                                                        nullptr,
                                                        nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 priv_ra8_esp_hosted_gpio_edge_register(RA8_PIN(k_ra8_port_4, k_ra8_pin_0),
                                                        (uint8_t)k_gpio_test_bad_sense,
                                                        internal_count_handler,
                                                        nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, priv_ra8_esp_hosted_gpio_set_edge_poll_ms(0U));
  TEST_ASSERT_EQ(k_ra8_ok,
                 priv_ra8_esp_hosted_gpio_set_edge_poll_ms((uint16_t)k_gpio_test_poll_ms));

  /* Port 4 has no ICU routing at all, so every one of these is polled. */
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    const ra8_port_pin_t pin = RA8_PIN(k_ra8_port_4, i);
    TEST_ASSERT_EQ(
      k_ra8_ok,
      priv_ra8_esp_hosted_gpio_edge_register(pin, sense, internal_count_handler, nullptr));
    TEST_ASSERT_EQ(
      k_ra8_err_exists,
      priv_ra8_esp_hosted_gpio_edge_register(pin, sense, internal_count_handler, nullptr));
  }
  TEST_ASSERT_EQ(k_ra8_esp_hosted_gpio_row_max, priv_ra8_esp_hosted_gpio_edge_count());

  const ra8_port_pin_t overflow = RA8_PIN(k_ra8_port_4, k_ra8_esp_hosted_gpio_row_max);
  TEST_ASSERT_EQ(
    k_ra8_err_no_mem,
    priv_ra8_esp_hosted_gpio_edge_register(overflow, sense, internal_count_handler, nullptr));

  TEST_ASSERT_EQ(k_ra8_err_not_found, priv_ra8_esp_hosted_gpio_edge_unregister(overflow));
  for (uint8_t i = 0U; i < (uint8_t)k_ra8_esp_hosted_gpio_row_max; i++) {
    TEST_ASSERT_EQ(k_ra8_ok, priv_ra8_esp_hosted_gpio_edge_unregister(RA8_PIN(k_ra8_port_4, i)));
  }
  TEST_ASSERT_EQ(0U, priv_ra8_esp_hosted_gpio_edge_count());
  TEST_END("esp_hosted gpio: the polled table fills up and then refuses");
}

/**
 * @par MC/DC:
 * (no compound decision -- this case takes both arms of the seam's
 * "something injected / nothing injected" test)
 * @brief Verify pin interface seam.
 * @details Drives the host model through pin interface seam and asserts each observable result.
 * @pre Unity is initialized. @pre Static fixture storage is available.
 * @post Expected outcomes are asserted. @post No host resource escapes this test.
 * @note Host-only deterministic fixture code; it does not access physical hardware.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_test_pin_interface_seam(void)
{
  TEST_BEGIN("esp_hosted gpio: the pin-driver seam falls back on the HAL");
  internal_reset_state();
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_pin_interface() == &s_mock_pin_if);
  priv_ra8_esp_hosted_gpio_set_pin_interface(nullptr);
  TEST_ASSERT_NOT_NULL((void*)priv_ra8_esp_hosted_gpio_pin_interface());
  TEST_ASSERT(priv_ra8_esp_hosted_gpio_pin_interface() != &s_mock_pin_if);
  TEST_ASSERT_NOT_NULL((void*)priv_ra8_esp_hosted_gpio_pin_interface()->read);
  priv_ra8_esp_hosted_gpio_set_pin_interface(&s_mock_pin_if);
  TEST_END("esp_hosted gpio: the pin-driver seam falls back on the HAL");
}

/** @brief Every case in this translation unit, in execution order. */
static void (*const s_test_roster[])(void) = {
  internal_test_decode_pin,
  internal_test_bind,
  internal_test_config_gpio,
  internal_test_read_write,
  internal_test_pull,
  internal_test_hold_and_wakeup_reason,
  internal_test_edge_seen_matrix,
  internal_test_hardware_interrupt_path,
  internal_test_interrupt_rejections,
  internal_test_polled_path,
  internal_test_polled_table_bounds,
  internal_test_pin_interface_seam,
};

int main(void)
{
  for (size_t i = 0U; i < (sizeof s_test_roster / sizeof s_test_roster[0]); ++i) {
    s_test_roster[i]();
  }
  return 0;
}
