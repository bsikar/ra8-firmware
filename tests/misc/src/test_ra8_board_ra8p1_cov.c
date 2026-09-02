/**
 * @file test_ra8_board_ra8p1_cov.c
 * @brief Host coverage tests for the RA8P1 foundation-board support layer.
 *
 * @details
 * Drives every board-coordinate lookup, HAL error propagation path, switch
 * selection, and UART console state through link-wrapped dependencies. The
 * wrappers are scoped to this executable; production code and every other
 * test continue to link the real HAL implementations.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "ra8_attributes.h"
#include "ra8_board_ra8p1.h"
#include "ra8_cgc.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_icu.h"
#include "ra8_isr.h"
#include "ra8_port_utils.h"
#include "ra8_sci.h"
#include "unity_minimal.h"

/** @brief Deterministic values used by the board wrapper fixture. */
typedef enum : uint32_t {
  k_fixture_baud           = 115200U,    /**< Valid console baud rate.             */
  k_fixture_pclka_hz       = 120000000U, /**< Valid post-PLL PCLKA frequency.      */
  k_fixture_low_pclka      = 8000000U,   /**< Reset clock below the console floor. */
  k_fixture_read_cap       = 3U,         /**< Console read-buffer capacity.        */
  k_fixture_isr_slot       = 7U,         /**< Slot published by the ISR mock.      */
  k_fixture_unwritten_slot = 0xFFFFU,    /**< Sentinel preserved on ISR failure.   */
  k_fixture_context        = 0x5AU,      /**< Callback context sentinel.           */
  k_fixture_invalid_id     = 99U,        /**< Value outside every board-id enum.   */
} ra8p1_fixture_value_t;

/** @struct ra8p1_mock_t
 * @brief Results, observations, and receive script for wrapped HAL calls.
 */
typedef struct {
  ra8_err_t         cgc_result;                 /**< Result returned by clock lookup.             */
  uint32_t          cgc_hz;                     /**< Frequency published by clock lookup.         */
  ra8_err_t         gpio_output_result;         /**< Result returned by GPIO output init.         */
  ra8_err_t         gpio_input_result;          /**< Result returned by GPIO input init.          */
  ra8_err_t         gpio_write_result;          /**< Result returned by GPIO write.               */
  ra8_err_t         gpio_toggle_result;         /**< Result returned by GPIO toggle.              */
  ra8_err_t         gpio_read_result;           /**< Result returned by GPIO read.                */
  ra8_level_t       gpio_read_level;            /**< Level published by GPIO read.                */
  ra8_err_t         icu_result;                 /**< Result returned by ICU configuration.        */
  ra8_err_t         isr_result;                 /**< Result returned by ISR registration.         */
  ra8_err_t         pfs_fail_result;            /**< Result injected on pfs_fail_call.            */
  uint32_t          pfs_fail_call;              /**< One-based route call to fail; zero for none. */
  ra8_err_t         sci_init_result;            /**< Result returned by SCI initialization.       */
  ra8_err_t         sci_write_result;           /**< Result returned by SCI write.                */
  ra8_err_t         sci_flush_result;           /**< Result returned by SCI flush.                */
  ra8_err_t         sci_getc_terminal;          /**< Result after scripted receive bytes.         */
  uint8_t           sci_rx[k_fixture_read_cap]; /**< Bytes published by SCI receive.              */
  size_t            sci_rx_count;               /**< Number of successful receive calls.          */
  size_t            sci_rx_index;               /**< Next scripted receive byte.                  */
  ra8_port_pin_t    last_pin;                   /**< Most recent GPIO pin argument.               */
  ra8_level_t       last_level;                 /**< Most recent GPIO level argument.             */
  ra8_pin_pull_t    last_pull;                  /**< Most recent GPIO pull argument.              */
  uint8_t           icu_irq;                    /**< Most recent ICU IRQ channel.                 */
  ra8_icu_irq_cfg_t icu_cfg;                    /**< Most recent ICU configuration.               */
  ra8_elc_event_t   isr_event;                  /**< Most recent ISR event.                       */
  ra8_isr_handler_t isr_handler;                /**< Most recent ISR callback.                    */
  void*             isr_ctx;                    /**< Most recent ISR callback context.            */
  uint8_t           isr_priority;               /**< Most recent ISR priority.                    */
  uint32_t          pfs_calls;                  /**< Number of PFS route calls.                   */
  ra8_port_pin_t    pfs_pins[2];                /**< Pins from the two console routes.            */
  ra8_psel_t        pfs_psels[2];               /**< PSEL values from console routes.             */
  const char*       pfs_owners[2];              /**< Owner strings from console routes.           */
  uint8_t           sci_channel;                /**< Most recent SCI channel.                     */
  ra8_sci_cfg_t     sci_cfg;                    /**< Most recent SCI configuration.               */
  const uint8_t*    sci_write_data;             /**< Most recent SCI write buffer.                */
  uint32_t          sci_write_len;              /**< Most recent SCI write length.                */
} ra8p1_mock_t;

/** @brief Per-process state consumed by the link-wrapped dependencies. */
static ra8p1_mock_t s_mock;

/** @brief Reset every wrapped dependency to a successful default. */
RA8_INTERNAL static void internal_reset_mock(void)
{
  s_mock = (ra8p1_mock_t){
    .cgc_result        = k_ra8_ok,
    .cgc_hz            = (uint32_t)k_fixture_pclka_hz,
    .sci_getc_terminal = k_ra8_err_not_found,
  };
}

/** @brief Harmless callback used to verify switch IRQ forwarding. */
RA8_INTERNAL static void internal_switch_callback(void* ctx)
{
  (void)ctx;
}

/** @brief Link wrapper for the board's clock query. */
RA8_TEST_HELPER ra8_err_t ra8p1_test_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz) __asm__(
  "__wrap_ra8_cgc_get_clock_hz");

RA8_TEST_HELPER ra8_err_t ra8p1_test_cgc_get_clock_hz(ra8_clock_id_t id, uint32_t* out_hz)
{
  TEST_ASSERT_EQ(k_ra8_clock_id_pclka, id);
  if (s_mock.cgc_result == k_ra8_ok) {
    *out_hz = s_mock.cgc_hz;
  }
  return s_mock.cgc_result;
}

/** @brief Link wrapper for GPIO output initialization. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_gpio_output_init(ra8_port_pin_t pin,
                            ra8_level_t    level) __asm__("__wrap_ra8_gpio_output_init");

RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_output_init(ra8_port_pin_t pin, ra8_level_t level)
{
  s_mock.last_pin   = pin;
  s_mock.last_level = level;
  return s_mock.gpio_output_result;
}

/** @brief Link wrapper for GPIO input initialization. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_gpio_input_init(ra8_port_pin_t pin,
                           ra8_pin_pull_t pull) __asm__("__wrap_ra8_gpio_input_init");

RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_input_init(ra8_port_pin_t pin, ra8_pin_pull_t pull)
{
  s_mock.last_pin  = pin;
  s_mock.last_pull = pull;
  return s_mock.gpio_input_result;
}

/** @brief Link wrapper for GPIO writes. */
RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_write(ra8_port_pin_t pin,
                                                ra8_level_t level) __asm__("__wrap_ra8_gpio_write");

RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_write(ra8_port_pin_t pin, ra8_level_t level)
{
  s_mock.last_pin   = pin;
  s_mock.last_level = level;
  return s_mock.gpio_write_result;
}

/** @brief Link wrapper for GPIO toggle. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_gpio_toggle(ra8_port_pin_t pin) __asm__("__wrap_ra8_gpio_toggle");

RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_toggle(ra8_port_pin_t pin)
{
  s_mock.last_pin = pin;
  return s_mock.gpio_toggle_result;
}

/** @brief Link wrapper for GPIO input sampling. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_gpio_read(ra8_port_pin_t pin, ra8_level_t* out_level) __asm__("__wrap_ra8_gpio_read");

RA8_TEST_HELPER ra8_err_t ra8p1_test_gpio_read(ra8_port_pin_t pin, ra8_level_t* out_level)
{
  s_mock.last_pin = pin;
  if (s_mock.gpio_read_result == k_ra8_ok) {
    *out_level = s_mock.gpio_read_level;
  }
  return s_mock.gpio_read_result;
}

/** @brief Link wrapper for external-IRQ pin configuration. */
RA8_TEST_HELPER ra8_err_t ra8p1_test_icu_configure_irq_pin(
  uint8_t                  irq_num,
  const ra8_icu_irq_cfg_t* cfg) __asm__("__wrap_ra8_icu_configure_irq_pin");

RA8_TEST_HELPER ra8_err_t ra8p1_test_icu_configure_irq_pin(uint8_t                  irq_num,
                                                           const ra8_icu_irq_cfg_t* cfg)
{
  s_mock.icu_irq = irq_num;
  s_mock.icu_cfg = *cfg;
  return s_mock.icu_result;
}

/** @brief Link wrapper for ISR registration. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_isr_register(ra8_elc_event_t   event,
                        ra8_isr_handler_t handler,
                        void*             ctx,
                        uint8_t           priority,
                        uint16_t*         out_slot) __asm__("__wrap_ra8_isr_register");

RA8_TEST_HELPER ra8_err_t ra8p1_test_isr_register(ra8_elc_event_t   event,
                                                  ra8_isr_handler_t handler,
                                                  void*             ctx,
                                                  uint8_t           priority,
                                                  uint16_t*         out_slot)
{
  s_mock.isr_event    = event;
  s_mock.isr_handler  = handler;
  s_mock.isr_ctx      = ctx;
  s_mock.isr_priority = priority;
  if (s_mock.isr_result != k_ra8_ok) {
    return s_mock.isr_result;
  }
  if (out_slot != nullptr) {
    *out_slot = (uint16_t)k_fixture_isr_slot;
  }

  return s_mock.isr_result;
}

/** @brief Link wrapper for console-pin routing. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_pfs_route(ra8_port_pin_t pin,
                     ra8_psel_t     psel,
                     const char*    owner) __asm__("__wrap_ra8_pfs_route_peripheral");

RA8_TEST_HELPER ra8_err_t ra8p1_test_pfs_route(ra8_port_pin_t pin,
                                               ra8_psel_t     psel,
                                               const char*    owner)
{
  if (s_mock.pfs_calls < 2U) {
    s_mock.pfs_pins[s_mock.pfs_calls]   = pin;
    s_mock.pfs_psels[s_mock.pfs_calls]  = psel;
    s_mock.pfs_owners[s_mock.pfs_calls] = owner;
  }
  s_mock.pfs_calls += 1U;
  return s_mock.pfs_fail_call == s_mock.pfs_calls ? s_mock.pfs_fail_result : k_ra8_ok;
}

/** @brief Link wrapper for SCI initialization. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg) __asm__("__wrap_ra8_sci_init");

RA8_TEST_HELPER ra8_err_t ra8p1_test_sci_init(uint8_t channel, const ra8_sci_cfg_t* cfg)
{
  s_mock.sci_channel = channel;
  s_mock.sci_cfg     = *cfg;
  return s_mock.sci_init_result;
}

/** @brief Link wrapper for SCI polling writes. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_sci_write(uint8_t        channel,
                     const uint8_t* data,
                     uint32_t       len) __asm__("__wrap_ra8_sci_write_polling");

RA8_TEST_HELPER ra8_err_t ra8p1_test_sci_write(uint8_t channel, const uint8_t* data, uint32_t len)
{
  s_mock.sci_channel    = channel;
  s_mock.sci_write_data = data;
  s_mock.sci_write_len  = len;
  return s_mock.sci_write_result;
}

/** @brief Link wrapper for SCI polling reads. */
RA8_TEST_HELPER ra8_err_t
ra8p1_test_sci_getc(uint8_t channel, uint8_t* out_byte) __asm__("__wrap_ra8_sci_getc_polling");

RA8_TEST_HELPER ra8_err_t ra8p1_test_sci_getc(uint8_t channel, uint8_t* out_byte)
{
  s_mock.sci_channel = channel;
  if (s_mock.sci_rx_index < s_mock.sci_rx_count) {
    *out_byte = s_mock.sci_rx[s_mock.sci_rx_index];
    s_mock.sci_rx_index += 1U;
    return k_ra8_ok;
  }
  return s_mock.sci_getc_terminal;
}

/** @brief Link wrapper for SCI flush. */
RA8_TEST_HELPER ra8_err_t ra8p1_test_sci_flush(uint8_t channel) __asm__("__wrap_ra8_sci_flush");

RA8_TEST_HELPER ra8_err_t ra8p1_test_sci_flush(uint8_t channel)
{
  s_mock.sci_channel = channel;
  return s_mock.sci_flush_result;
}

/** @brief Exercise every API guard that depends on the console init latch. */
RA8_INTERNAL static void internal_test_preinit_guards(void)
{
  TEST_BEGIN("RA8P1 console rejects I/O before initialization");
  internal_reset_mock();
  uint8_t data = 0x41U;
  size_t  len  = (size_t)k_fixture_read_cap;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_write(nullptr, 0U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_write(nullptr, 1U));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_write(&data, 1U));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_read(&data, 1U, nullptr));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_read(nullptr, 0U, &len));
  TEST_ASSERT_EQ(0U, len);
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_read(nullptr, 1U, &len));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_read(&data, 1U, &len));
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_flush());
  TEST_END("RA8P1 console rejects I/O before initialization");
}

/** @brief Exercise identity and every LED/switch lookup validation arm. */
RA8_INTERNAL static void internal_test_identity_and_pin_maps(void)
{
  TEST_BEGIN("RA8P1 identity and pin tables are complete");
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_get_info(nullptr));
  ra8_board_info_t info = {};
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_get_info(&info));
  TEST_ASSERT(strcmp(info.name, "RA8P1 foundation board") == 0);
  TEST_ASSERT(strcmp(info.doc_rev, "R01UH1064EJ (chip HUM)") == 0);
  TEST_ASSERT(strcmp(info.mcu, "R7KA8P1KFLCAC") == 0);

  ra8_port_pin_t pin = k_ra8_pin_none;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_pin(k_ra8_board_led1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_led_pin((ra8_board_led_id_t)k_fixture_invalid_id, &pin));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led1, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_0), pin);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led2, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_3, k_ra8_pin_3), pin);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_pin(k_ra8_board_led3, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_10, k_ra8_pin_7), pin);

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_pin(k_ra8_board_sw1, nullptr));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_sw_pin((ra8_board_sw_id_t)k_fixture_invalid_id, &pin));
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_pin(k_ra8_board_sw1, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_9), pin);
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_pin(k_ra8_board_sw2, &pin));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_0, k_ra8_pin_8), pin);
  TEST_END("RA8P1 identity and pin tables are complete");
}

/** @brief Exercise LED wrapper success and dependency-failure forwarding. */
RA8_INTERNAL static void internal_test_led_wrappers(void)
{
  TEST_BEGIN("RA8P1 LED wrappers preserve ids, pins, levels, and errors");
  internal_reset_mock();
  const ra8_board_led_id_t invalid = (ra8_board_led_id_t)k_fixture_invalid_id;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_init(invalid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_on(invalid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_off(invalid));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_led_toggle(invalid));

  s_mock.gpio_output_result = k_ra8_err_gpio_conflict;
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_led_init(k_ra8_board_led1));
  TEST_ASSERT_EQ(RA8_PIN(k_ra8_port_6, k_ra8_pin_0), s_mock.last_pin);
  TEST_ASSERT_EQ(k_ra8_level_low, s_mock.last_level);
  s_mock.gpio_output_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_init(k_ra8_board_led2));

  s_mock.gpio_write_result = k_ra8_err_hw_unmapped;
  TEST_ASSERT_EQ(k_ra8_err_hw_unmapped, ra8_board_led_on(k_ra8_board_led3));
  TEST_ASSERT_EQ(k_ra8_level_high, s_mock.last_level);
  s_mock.gpio_write_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_off(k_ra8_board_led3));
  TEST_ASSERT_EQ(k_ra8_level_low, s_mock.last_level);

  s_mock.gpio_toggle_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_led_toggle(k_ra8_board_led2));
  s_mock.gpio_toggle_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_led_toggle(k_ra8_board_led1));
  TEST_END("RA8P1 LED wrappers preserve ids, pins, levels, and errors");
}

/** @brief Exercise switch setup, active-low reads, and dependency errors. */
RA8_INTERNAL static void internal_test_switch_io(void)
{
  TEST_BEGIN("RA8P1 switch wrappers preserve active-low semantics");
  internal_reset_mock();
  const ra8_board_sw_id_t invalid = (ra8_board_sw_id_t)k_fixture_invalid_id;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_init(invalid));
  s_mock.gpio_input_result = k_ra8_err_gpio_conflict;
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_sw_init(k_ra8_board_sw1));
  TEST_ASSERT_EQ(k_ra8_pull_up, s_mock.last_pull);
  s_mock.gpio_input_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_init(k_ra8_board_sw2));

  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_read(k_ra8_board_sw1, nullptr));
  ra8_board_sw_state_t state = k_ra8_board_sw_pressed;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_read(invalid, &state));
  s_mock.gpio_read_result = k_ra8_err_hw_unmapped;
  TEST_ASSERT_EQ(k_ra8_err_hw_unmapped, ra8_board_sw_read(k_ra8_board_sw1, &state));
  s_mock.gpio_read_result = k_ra8_ok;
  s_mock.gpio_read_level  = k_ra8_level_high;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_read(k_ra8_board_sw1, &state));
  TEST_ASSERT_EQ(k_ra8_board_sw_released, state);
  s_mock.gpio_read_level = k_ra8_level_low;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_read(k_ra8_board_sw2, &state));
  TEST_ASSERT_EQ(k_ra8_board_sw_pressed, state);
  TEST_END("RA8P1 switch wrappers preserve active-low semantics");
}

/** @brief Exercise callback validation, IRQ selection, and both dependencies. */
RA8_INTERNAL static void internal_test_switch_irq(void)
{
  TEST_BEGIN("RA8P1 switch IRQ routing selects the matching ICU event");
  internal_reset_mock();
  void* const ctx = (void*)(uintptr_t)k_fixture_context;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_sw_attach_irq(k_ra8_board_sw1, nullptr, ctx));
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg,
                 ra8_board_sw_attach_irq((ra8_board_sw_id_t)k_fixture_invalid_id,
                                         internal_switch_callback,
                                         ctx));
  s_mock.icu_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout,
                 ra8_board_sw_attach_irq(k_ra8_board_sw1, internal_switch_callback, ctx));
  s_mock.icu_result = k_ra8_ok;
  s_mock.isr_result = k_ra8_err_no_mem;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8_board_sw_attach_irq(k_ra8_board_sw1, internal_switch_callback, ctx));
  TEST_ASSERT_EQ(k_ra8_board_sw1_irq, s_mock.icu_irq);
  TEST_ASSERT_EQ(k_ra8_elc_event_icu_irq13, s_mock.isr_event);

  s_mock.isr_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_sw_attach_irq(k_ra8_board_sw2, internal_switch_callback, ctx));
  TEST_ASSERT_EQ(k_ra8_board_sw2_irq, s_mock.icu_irq);
  TEST_ASSERT_EQ(k_ra8_icu_irqmd_falling, s_mock.icu_cfg.sense);
  TEST_ASSERT_EQ(k_ra8_icu_fclksel_pclkb, s_mock.icu_cfg.filter_div);
  TEST_ASSERT(s_mock.icu_cfg.filter_en);
  TEST_ASSERT_EQ(k_ra8_elc_event_icu_irq12, s_mock.isr_event);
  TEST_ASSERT(s_mock.isr_handler == (ra8_isr_handler_t)internal_switch_callback);
  TEST_ASSERT(s_mock.isr_ctx == ctx);
  TEST_ASSERT_EQ(k_ra8_isr_prio_default, s_mock.isr_priority);
  TEST_END("RA8P1 switch IRQ routing selects the matching ICU event");
}

/** @brief Verify ISR registration publishes a slot only after successful registration. */
RA8_INTERNAL static void internal_test_isr_out_slot_contract(void)
{
  TEST_BEGIN("RA8P1 ISR wrapper preserves the production out-slot contract");
  internal_reset_mock();
  void* const ctx  = (void*)(uintptr_t)k_fixture_context;
  uint16_t    slot = (uint16_t)k_fixture_unwritten_slot;

  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8p1_test_isr_register(k_ra8_elc_event_icu_irq13,
                                         internal_switch_callback,
                                         ctx,
                                         k_ra8_isr_prio_default,
                                         &slot));
  TEST_ASSERT_EQ(k_fixture_isr_slot, slot);

  s_mock.isr_result = k_ra8_err_no_mem;
  slot              = (uint16_t)k_fixture_unwritten_slot;
  TEST_ASSERT_EQ(k_ra8_err_no_mem,
                 ra8p1_test_isr_register(k_ra8_elc_event_icu_irq13,
                                         internal_switch_callback,
                                         ctx,
                                         k_ra8_isr_prio_default,
                                         &slot));
  TEST_ASSERT_EQ(k_fixture_unwritten_slot, slot);

  internal_reset_mock();
  TEST_ASSERT_EQ(k_ra8_ok,
                 ra8p1_test_isr_register(k_ra8_elc_event_icu_irq13,
                                         internal_switch_callback,
                                         ctx,
                                         k_ra8_isr_prio_default,
                                         nullptr));
  TEST_END("RA8P1 ISR wrapper preserves the production out-slot contract");
}

/** @brief Exercise every console initialization error before the successful latch. */
RA8_INTERNAL static void internal_test_uart_init_errors(void)
{
  TEST_BEGIN("RA8P1 console init propagates each dependency failure");
  internal_reset_mock();
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_init(0U));
  s_mock.cgc_result = k_ra8_err_invalid_arg;
  TEST_ASSERT_EQ(k_ra8_err_invalid_arg, ra8_board_uart_console_init(k_fixture_baud));
  internal_reset_mock();
  s_mock.cgc_hz = (uint32_t)k_fixture_low_pclka;
  TEST_ASSERT_EQ(k_ra8_err_not_initialized, ra8_board_uart_console_init(k_fixture_baud));

  internal_reset_mock();
  s_mock.pfs_fail_call   = 1U;
  s_mock.pfs_fail_result = k_ra8_err_gpio_conflict;
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_uart_console_init(k_fixture_baud));
  internal_reset_mock();
  s_mock.pfs_fail_call   = 2U;
  s_mock.pfs_fail_result = k_ra8_err_gpio_conflict;
  TEST_ASSERT_EQ(k_ra8_err_gpio_conflict, ra8_board_uart_console_init(k_fixture_baud));
  TEST_ASSERT_EQ(2U, s_mock.pfs_calls);

  internal_reset_mock();
  s_mock.sci_init_result = k_ra8_err_hw_init_failed;
  TEST_ASSERT_EQ(k_ra8_err_hw_init_failed, ra8_board_uart_console_init(k_fixture_baud));
  TEST_END("RA8P1 console init propagates each dependency failure");
}

/** @brief Initialize the console and verify its exact route and SCI descriptor. */
RA8_INTERNAL static void internal_test_uart_init_success(void)
{
  TEST_BEGIN("RA8P1 console init forwards the runtime clock and route");
  internal_reset_mock();
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_init(k_fixture_baud));
  TEST_ASSERT_EQ(2U, s_mock.pfs_calls);
  TEST_ASSERT_EQ(k_ra8_board_uart_console_pin_txd, s_mock.pfs_pins[0]);
  TEST_ASSERT_EQ(k_ra8_board_uart_console_pin_rxd, s_mock.pfs_pins[1]);
  TEST_ASSERT_EQ(k_ra8_psel_sci_async, s_mock.pfs_psels[0]);
  TEST_ASSERT_EQ(k_ra8_psel_sci_async, s_mock.pfs_psels[1]);
  TEST_ASSERT(strcmp("ra8_board.uart.console.txd", s_mock.pfs_owners[0]) == 0);
  TEST_ASSERT(strcmp("ra8_board.uart.console.rxd", s_mock.pfs_owners[1]) == 0);
  TEST_ASSERT_EQ(k_ra8_board_uart_console_sci_channel, s_mock.sci_channel);
  TEST_ASSERT_EQ(k_fixture_baud, s_mock.sci_cfg.baud);
  TEST_ASSERT_EQ(k_ra8_sci_data_8, s_mock.sci_cfg.data_bits);
  TEST_ASSERT_EQ(k_ra8_sci_parity_none, s_mock.sci_cfg.parity);
  TEST_ASSERT_EQ(k_ra8_sci_stop_1, s_mock.sci_cfg.stop_bits);
  TEST_ASSERT_EQ(k_fixture_pclka_hz, s_mock.sci_cfg.pclk_hz);
  TEST_END("RA8P1 console init forwards the runtime clock and route");
}

/** @brief Exercise initialized console writes and flush result forwarding. */
RA8_INTERNAL static void internal_test_uart_write_and_flush(void)
{
  TEST_BEGIN("RA8P1 initialized console forwards write and flush results");
  internal_reset_mock();
  const uint8_t data[]    = {0x41U, 0x42U};
  s_mock.sci_write_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_uart_console_write(data, sizeof(data)));
  TEST_ASSERT(s_mock.sci_write_data == data);
  TEST_ASSERT_EQ(sizeof(data), s_mock.sci_write_len);
  TEST_ASSERT_EQ(k_ra8_board_uart_console_sci_channel, s_mock.sci_channel);
  s_mock.sci_write_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_write(data, sizeof(data)));

  s_mock.sci_flush_result = k_ra8_err_hw_timeout;
  TEST_ASSERT_EQ(k_ra8_err_hw_timeout, ra8_board_uart_console_flush());
  s_mock.sci_flush_result = k_ra8_ok;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_flush());
  TEST_END("RA8P1 initialized console forwards write and flush results");
}

/** @brief Exercise console read early-stop and full-capacity loop exits. */
RA8_INTERNAL static void internal_test_uart_read(void)
{
  TEST_BEGIN("RA8P1 console read drains available bytes up to capacity");
  internal_reset_mock();
  uint8_t out[k_fixture_read_cap] = {};
  size_t  out_len                 = (size_t)k_fixture_read_cap;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_read(out, sizeof(out), &out_len));
  TEST_ASSERT_EQ(0U, out_len);

  internal_reset_mock();
  s_mock.sci_rx[0]    = 0x31U;
  s_mock.sci_rx[1]    = 0x32U;
  s_mock.sci_rx_count = 2U;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_read(out, sizeof(out), &out_len));
  TEST_ASSERT_EQ(2U, out_len);
  TEST_ASSERT_EQ(0x31U, out[0]);
  TEST_ASSERT_EQ(0x32U, out[1]);

  internal_reset_mock();
  s_mock.sci_rx[0]    = 0x61U;
  s_mock.sci_rx[1]    = 0x62U;
  s_mock.sci_rx[2]    = 0x63U;
  s_mock.sci_rx_count = (size_t)k_fixture_read_cap;
  TEST_ASSERT_EQ(k_ra8_ok, ra8_board_uart_console_read(out, sizeof(out), &out_len));
  TEST_ASSERT_EQ(k_fixture_read_cap, out_len);
  TEST_ASSERT_EQ(0x63U, out[2]);
  TEST_END("RA8P1 console read drains available bytes up to capacity");
}

int main(void)
{
  internal_test_preinit_guards();
  internal_test_identity_and_pin_maps();
  internal_test_led_wrappers();
  internal_test_switch_io();
  internal_test_switch_irq();
  internal_test_isr_out_slot_contract();
  internal_test_uart_init_errors();
  internal_test_uart_init_success();
  internal_test_uart_write_and_flush();
  internal_test_uart_read();
  return 0;
}
