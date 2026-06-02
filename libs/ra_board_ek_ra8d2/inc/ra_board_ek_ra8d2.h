/**
 * @file ra_board_ek_ra8d2.h
 * @brief Board-support layer for the Renesas EK-RA8D2 v1 evaluation kit
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Names every on-board feature of the EK-RA8D2 v1 (LEDs, switches,
 * connectors, peripherals) so application code can speak in board
 * coordinates ("LED1", "Arduino D13", "Pmod1 SCK") rather than chip
 * coordinates ("P600", "P102", "P803"). Every pin enum carries a
 * citation back to a numbered table in the EK-RA8D2 v1 User's Manual
 * so a reader can audit the wiring without opening the schematic.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * Underlying chip register access is delegated to ``libs/ra_hal``:
 * the BSP itself does NO register pokes -- it just translates board
 * names into the right ``ra_port_pin_t`` / ``ra_psel_t`` /
 * ``ra_icu_irq_cfg_t`` values and forwards to the HAL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra_err.h"
#include "ra_port_constants.h"

#ifdef __cplusplus
extern "C" {
#endif

/* =============================================================================
 * Board identity
 * =============================================================================
 */

/**
 * @brief Human-readable strings identifying this BSP target.
 *
 * @details
 * Returned by ``ra_board_get_info()`` so applications can log /
 * verify the board they were built for. All three are ASCII string
 * literals with permanent storage duration.
 */
extern const char* const k_ra_board_name;    /**< "EK-RA8D2 v1". */
extern const char* const k_ra_board_doc_rev; /**< "R20UT5523EG0101 Rev 1.01". */
extern const char* const k_ra_board_mcu;     /**< "R7KA8D2KFLCAC". */

/**
 * @struct ra_board_info_t
 * @brief Snapshot of board identity strings.
 */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  const char* name;    /**< Same value as ``k_ra_board_name``.    */
  const char* doc_rev; /**< Same value as ``k_ra_board_doc_rev``. */
  const char* mcu;     /**< Same value as ``k_ra_board_mcu``.     */
} ra_board_info_t;
/* cppcheck-suppress-end unusedStructMember */

/**
 * @brief Copy the three board-identity strings into ``out``.
 *
 * @param[out] out Destination struct, must be non-NULL.
 *
 * @retval k_ra_ok                 Filled.
 * @retval k_ra_err_invalid_arg    out == NULL.
 *
 * @pre out is writable.
 * @post On success out->name / doc_rev / mcu point at the global
 *       ``k_ra_board_*`` strings.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_get_info(ra_board_info_t* out);

/* =============================================================================
 * 1. User LEDs (UM Section 5.5.1, Table 24, page 31)
 * =============================================================================
 */

/**
 * @enum ra_board_led_id_t
 * @brief Identifiers for the three user LEDs on EK-RA8D2 v1.
 *
 * @details
 * Pins per UM Table 24 ("EK-RA8D2 Board LED Functions"). Trace-cut
 * jumpers E27/E26/E28 (closed by default) tie the LEDs to P600/P303/PA07
 * respectively. All three LEDs are active-high.
 */
typedef enum : uint8_t {
  k_ra_board_led1 = 0U, /**< LED1, BLUE,  P600 (jumper E27). EK-RA8D2 UM Table 24 p 31. */
  k_ra_board_led2 = 1U, /**< LED2, GREEN, P303 (jumper E26). EK-RA8D2 UM Table 24 p 31. */
  k_ra_board_led3 = 2U, /**< LED3, RED,   PA07 (jumper E28). EK-RA8D2 UM Table 24 p 31. */

  /** Convenience aliases by colour. */
  k_ra_board_led_blue  = k_ra_board_led1,
  k_ra_board_led_green = k_ra_board_led2,
  k_ra_board_led_red   = k_ra_board_led3,

  k_ra_board_led_count = 3U,
} ra_board_led_id_t;

/**
 * @brief Configure ``led`` as a digital output, initial level low (off).
 *
 * @retval k_ra_ok                  Pin claimed and driven low.
 * @retval k_ra_err_invalid_arg     led >= k_ra_board_led_count.
 * @retval k_ra_err_gpio_conflict   Pin already owned.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @post LED is off and pin is configured as a digital output.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_led_init(ra_board_led_id_t led);

/**
 * @brief Drive ``led`` HIGH (light it).
 * @retval k_ra_ok / k_ra_err_invalid_arg
 * @pre ra_board_led_init(led) succeeded.
 * @post LED pin output latch == 1.
 */
[[nodiscard]] ra_err_t ra_board_led_on(ra_board_led_id_t led);

/**
 * @brief Drive ``led`` LOW (extinguish it).
 * @retval k_ra_ok / k_ra_err_invalid_arg
 * @pre ra_board_led_init(led) succeeded.
 * @post LED pin output latch == 0.
 */
[[nodiscard]] ra_err_t ra_board_led_off(ra_board_led_id_t led);

/**
 * @brief Toggle ``led``'s output state.
 * @retval k_ra_ok / k_ra_err_invalid_arg
 * @pre ra_board_led_init(led) succeeded.
 * @post LED pin output latch is inverted from its prior value.
 */
[[nodiscard]] ra_err_t ra_board_led_toggle(ra_board_led_id_t led);

/**
 * @brief Translate a board LED id into its underlying ``ra_port_pin_t``.
 *
 * @param[in]  led      LED identifier.
 * @param[out] out_pin  Destination pin id.
 * @retval k_ra_ok / k_ra_err_invalid_arg
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_led_pin(ra_board_led_id_t led, ra_port_pin_t* out_pin);

/* =============================================================================
 * 2. User switches (UM Section 5.5.2, Table 25, page 32)
 * =============================================================================
 */

/**
 * @enum ra_board_sw_id_t
 * @brief Identifiers for the on-board user push-buttons.
 *
 * @details
 * Pins per UM Table 25 ("EK-RA8D2 Board Switches"). SW3 is tied to
 * the chip RESET_L line and is therefore not exposed as a
 * software-readable input here.
 */
typedef enum : uint8_t {
  k_ra_board_sw1      = 0U, /**< SW1, P009, IRQ13-DS (jumper E31). EK-RA8D2 UM Table 25 p 32. */
  k_ra_board_sw2      = 1U, /**< SW2, P008, IRQ12-DS (jumper E32). EK-RA8D2 UM Table 25 p 32. */
  k_ra_board_sw_count = 2U,
} ra_board_sw_id_t;

/**
 * @brief Per-switch IRQ channel numbers (UM Table 25 p 32).
 *
 * @details
 * SW1 -> IRQ13-DS, SW2 -> IRQ12-DS. The "-DS" suffix means
 * deep-sleep wake capable. Values are bare IRQ channel numbers
 * usable with ``ra_icu_configure_irq_pin``.
 */
typedef enum : uint8_t {
  k_ra_board_sw1_irq = 13U, /**< SW1 -> IRQ13-DS. UM Table 25 p 32. */
  k_ra_board_sw2_irq = 12U, /**< SW2 -> IRQ12-DS. UM Table 25 p 32. */
} ra_board_sw_irq_t;

/** @brief Logical "pressed" / "released" state. */
typedef enum : uint8_t {
  k_ra_board_sw_released = 0U,
  k_ra_board_sw_pressed  = 1U,
} ra_board_sw_state_t;

/**
 * @brief Configure a switch pin as an input with internal pull-up.
 *
 * @retval k_ra_ok / k_ra_err_invalid_arg / k_ra_err_gpio_conflict
 * @pre HAL pin validator initialized.
 * @post Pin is digital input, pull-up enabled.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_sw_init(ra_board_sw_id_t sw);

/**
 * @brief Sample the current state of ``sw``.
 *
 * @details
 * The buttons are wired active-low (press shorts the pin to GND);
 * this function inverts the level so callers receive a positive
 * "pressed = true" semantic.
 *
 * @param[in]  sw           Switch id.
 * @param[out] out_pressed  Set to ``k_ra_board_sw_pressed`` if held.
 *
 * @retval k_ra_ok                 Read complete.
 * @retval k_ra_err_invalid_arg    sw out of range or out_pressed NULL.
 *
 * @pre ra_board_sw_init(sw) succeeded.
 * @post *out_pressed is one of {released, pressed}.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_sw_read(ra_board_sw_id_t sw, ra_board_sw_state_t* out_pressed);

/** @brief Switch IRQ callback signature. */
typedef void (*ra_board_sw_irq_cb_t)(void* ctx);

/**
 * @brief Wire ``sw`` to the ICU and register a falling-edge callback.
 *
 * @details
 * Buttons are active-low so a falling edge corresponds to a press.
 * The HAL takes care of the PFS routing, IRQCR programming, and
 * NVIC enable; this veneer just hides the specific channel number
 * (UM Table 25) from the caller.
 *
 * @retval k_ra_ok / k_ra_err_invalid_arg / k_ra_err_gpio_conflict
 * @pre ra_icu_init() has been called once during boot.
 * @post Pressing ``sw`` invokes ``cb(ctx)`` from the ICU ISR context.
 */
[[nodiscard]] ra_err_t
ra_board_sw_attach_irq(ra_board_sw_id_t sw, ra_board_sw_irq_cb_t cb, void* ctx);

/**
 * @brief Translate a board switch id into its underlying ``ra_port_pin_t``.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_sw_pin(ra_board_sw_id_t sw, ra_port_pin_t* out_pin);

/* =============================================================================
 * 3. Parallel-RGB Graphics Expansion connector J1
 *    (UM Section 8.1, Table 33, page 42)
 * =============================================================================
 */

/** @brief GLCDC parallel-RGB pixel formats supported by EK-RA8D2 J1. */
typedef enum : uint8_t {
  k_ra_board_glcdc_fmt_rgb888 = 0U, /**< 24-bit, 8 bits/colour.   */
  k_ra_board_glcdc_fmt_rgb666 = 1U, /**< 18-bit, 6 bits/colour.   */
  k_ra_board_glcdc_fmt_rgb565 = 2U, /**< 16-bit, 5/6/5 bits.      */
} ra_board_glcdc_fmt_t;

/** @brief One row of the GLCDC pin table -- (signal name, RA8D2 pin). */
/* cppcheck-suppress-begin unusedStructMember */
typedef struct {
  const char*   signal; /**< Human-readable signal name (UM Table 33). */
  ra_port_pin_t pin;    /**< RA8D2 port pin carrying that signal.      */
} ra_board_glcdc_pin_t;
/* cppcheck-suppress-end unusedStructMember */

/**
 * @brief Pin tables for J1 in each pixel format (UM Table 33 p 42).
 *
 * @details
 * Source-of-truth arrays the BSP iterates when programming J1. The
 * tables are exposed publicly so applications can also inspect them
 * for documentation / debug purposes. Only the data lines actually
 * routed in the chosen format appear in the per-format table; common
 * control + side-band signals (BLEN, RST, INT, I2C, CLK, sync, EXTCLK,
 * TCON3) appear in every table.
 */
extern const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb888_pins[];
extern const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb666_pins[];
extern const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb565_pins[];

/** @brief Number of entries in each per-format pin table. */
extern const uint32_t g_ra_board_glcdc_rgb888_pin_count;
extern const uint32_t g_ra_board_glcdc_rgb666_pin_count;
extern const uint32_t g_ra_board_glcdc_rgb565_pin_count;

/**
 * @brief Program every J1 pin for ``fmt`` to its GLCDC alternate function.
 *
 * @details
 * Walks the right table above and calls ``ra_pfs_route_peripheral``
 * for each entry. Does NOT bring the GLCDC peripheral itself up --
 * call ``ra_glcdc_init`` after this returns.
 *
 * @retval k_ra_ok                 All pins routed.
 * @retval k_ra_err_invalid_arg    fmt out of range.
 * @retval k_ra_err_gpio_conflict  At least one pin already owned.
 *
 * @pre IOPORT module powered (true at reset).
 * @post Every J1 pin for the chosen format is in GLCDC mode.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_glcdc_init(ra_board_glcdc_fmt_t fmt);

/** @brief Board-style aliases for the J1 LCD GPIO control pins.
 *
 *  The underlying ``ra_port_pin_t`` enum members live in
 *  ``libs/ra_core/inc/ra_port_constants.h`` (the same place LED1/2/3
 *  are declared) so casting them to ``ra_port_pin_t`` is type-safe
 *  for the analyzer.  Source: EK-RA8D2 v1 UM Table 33 p 42.
 */
typedef enum : uint16_t {
  k_ra_board_lcd_reset_l = (uint16_t)k_ra_pin_lcd_reset_l, /**< J1-6 RST,  P606 (active-low). */
  k_ra_board_lcd_blen    = (uint16_t)k_ra_pin_lcd_blen,    /**< J1-1 BLEN, P514 (active-high). */
} ra_board_lcd_gpio_pin_t;

/**
 * @brief Pulse the J1 panel's RESET_L line and assert BLEN backlight.
 *
 * @details Drives `k_ra_board_lcd_reset_l` low for 50 ms, releases it
 * high, waits another 50 ms for the panel's internal controller to
 * latch reset-release, then drives `k_ra_board_lcd_blen` high to
 * enable the backlight.  Must be called BEFORE `ra_board_glcdc_init`
 * routes the data pins to PSEL=glcdc so the panel sees stable signals
 * by the time GLCDC starts driving them.
 *
 * @retval k_ra_ok               Panel reset released and backlight on.
 * @retval k_ra_err_invalid_arg  GPIO init for one of the pins failed.
 * @retval k_ra_err_gpio_*       Underlying ra_gpio_* propagated error.
 *
 * @pre IOPORT module is powered (true at reset).
 * @pre `ra_time_init` has been called (function blocks on `ra_delay_ms`).
 * @post P606 (RESET_L) is driven high; panel is out of reset.
 * @post P514 (BLEN) is driven high; backlight is on.
 *
 * @note Single-threaded init context; blocks for ~100 ms total.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_lcd_panel_power_on(void);

/* =============================================================================
 * 4. Audio CODEC (DA7212 U14, SSIE + I2C)
 *    (UM Section 6.6, Table 32, page 38)
 * =============================================================================
 */

/**
 * @enum ra_board_audio_pin_t
 * @brief Pin assignments for the on-board DA7212 audio CODEC.
 *
 * @details
 * Per UM Table 32 ("Audio CODEC Port Pin Assignments"). P405/P406
 * are shared with the parallel-camera connector via jumper J41;
 * the camera and CODEC cannot be used simultaneously.
 */
typedef enum : uint16_t {
  k_ra_board_audio_pin_bclk =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_3), /**< P403, EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_wclk =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_4), /**< P404, EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_datin =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_5), /**< P405 (J41), EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_datout =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_6), /**< P406 (J41), EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_mclk =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_6), /**< PD06, EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_i2c_sda =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_11), /**< P511 (SDA1), EK-RA8D2 UM Table 32 p 38. */
  k_ra_board_audio_pin_i2c_scl =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_12), /**< P512 (SCL1), EK-RA8D2 UM Table 32 p 38. */
} ra_board_audio_pin_t;

/**
 * @brief SSIE channel the CODEC is wired to.
 *
 * @details
 * P403..P406 carry SSIE0 signals on the RA8D2 (see chip HUM I/O Ports
 * chapter for PSEL values). Channel selection itself is not in the
 * EK-RA8D2 board manual, only the chip pins.
 */
typedef enum : uint8_t {
  k_ra_board_audio_ssie_channel = 0U,
} ra_board_audio_ch_t;

/**
 * @brief Route the CODEC pins and bring up SSIE0 in I2S controller mode.
 *
 * @retval k_ra_ok / k_ra_err_invalid_arg / k_ra_err_gpio_conflict
 *
 * @pre J41 jumpers populated (CODEC selected over camera).
 * @post All CODEC pins are routed; SSIE0 is enabled.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_board_audio_init(uint32_t sample_rate_hz, uint8_t bit_depth, uint8_t channels);

/**
 * @brief Push one block of stereo PCM samples to the CODEC's DAC.
 *
 * @param[in] buf  Sample buffer (interleaved L/R, 16-bit).
 * @param[in] len  Number of samples (must be even).
 *
 * @retval k_ra_ok                  Block enqueued or transmitted.
 * @retval k_ra_err_invalid_arg     buf NULL or len odd / zero.
 * @retval k_ra_err_not_initialized ra_board_audio_init not called.
 *
 * @pre ra_board_audio_init succeeded.
 * @post Sample block has been handed to SSIE0.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_audio_play_sample_block(const int16_t* buf, uint32_t len);

/* =============================================================================
 * 5. Arduino Uno header (J18/J19/J23/J24) (UM Section 5.3.4, Table 20, p 27-28)
 * =============================================================================
 */

/**
 * @enum ra_board_arduino_pin_t
 * @brief Logical Arduino-header pins mapped to RA8D2 port pins.
 *
 * @details
 * Per UM Table 20 ("Arduino Uno Port Assignments"). Several pins are
 * gated by SW4-4 (Arduino vs Octo-SPI). PWM/GTIOC routings noted in
 * UM Table 20 are reflected in the comments. SW4-4 must be ON to
 * use the Arduino headers (default OFF prioritises Octo-SPI).
 */
typedef enum : uint16_t {
  /* Digital I/O (J23 + J24). */
  k_ra_board_arduino_d0 =
    (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_8), /**< D0 RXD7/GTIOC13B, P808. UM Table 20 p 28. */
  k_ra_board_arduino_d1 =
    (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_9), /**< D1 TXD7,           P809. UM Table 20 p 28. */
  k_ra_board_arduino_d2 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_11), /**< D2 INT0/IRQ16,     P011. UM Table 20 p 28. */
  k_ra_board_arduino_d3 =
    (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_11), /**< D3 INT1/GTIOC10B,  P811. UM Table 20 p 28. */
  k_ra_board_arduino_d4 =
    (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_10), /**< D4 GTIOC10A/IRQ21, P810. UM Table 20 p 28. */
  k_ra_board_arduino_d5 =
    (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_4), /**< D5 GTIOC1B/IRQ1,   P104. UM Table 20 p 28. */
  k_ra_board_arduino_d6 =
    (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_5), /**< D6 GTIOC1A/IRQ0,   P105. UM Table 20 p 28. */
  k_ra_board_arduino_d7 =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_12), /**< D7 IRQ22-DS,       P312. UM Table 20 p 28. */
  k_ra_board_arduino_d8 =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_1), /**< D8 IRQ22,          PD01. UM Table 20 p 28. */
  k_ra_board_arduino_d9 =
    (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_10), /**< D9 GTIOC9B/IRQ20,  P110. UM Table 20 p 28. */
  k_ra_board_arduino_d10 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_3), /**< D10 SPI_SS / GTIOC2A,  P103. UM Table 20 p 28. */
  k_ra_board_arduino_d11 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_1), /**< D11 SPI_MOSI / GTIOC8A,P101. UM Table 20 p 28. */
  k_ra_board_arduino_d12 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_0), /**< D12 SPI_MISO / GTIOC8B,P100. UM Table 20 p 28. */
  k_ra_board_arduino_d13 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_2), /**< D13 SPI_SCK / GTIOC2B, P102. UM Table 20 p 28. */
  k_ra_board_arduino_d14 = (uint16_t)RA_PIN(
    k_ra_port_5,
    k_ra_pin_11), /**< D14 SDA1 (or SDA0=P401 if SW4-5 ON), P511. UM Table 20 p 28. */
  k_ra_board_arduino_d15 = (uint16_t)RA_PIN(
    k_ra_port_5,
    k_ra_pin_12), /**< D15 SCL1 (or SCL0=P400 if SW4-5 ON), P512. UM Table 20 p 28. */

  /* Analog input block (J19). */
  k_ra_board_arduino_a0 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_1), /**< A0 AN001, P001. UM Table 20 p 28. */
  k_ra_board_arduino_a1 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_7), /**< A1 AN007, P007. UM Table 20 p 28. */
  k_ra_board_arduino_a2 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_3), /**< A2 AN003, P003. UM Table 20 p 28. */
  k_ra_board_arduino_a3 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_4), /**< A3 AN004, P004. UM Table 20 p 28. */
  k_ra_board_arduino_a4 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_14), /**< A4 AN014/DA0, P014. UM Table 20 p 28. */
  k_ra_board_arduino_a5 =
    (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_15), /**< A5 AN015/DA1, P015. UM Table 20 p 28. */
} ra_board_arduino_pin_t;

/** @brief Direction mode for an Arduino-header GPIO pin. */
typedef enum : uint8_t {
  k_ra_board_arduino_mode_input        = 0U,
  k_ra_board_arduino_mode_input_pullup = 1U,
  k_ra_board_arduino_mode_output       = 2U,
} ra_board_arduino_mode_t;

/**
 * @brief Configure an Arduino header pin in GPIO ``mode``.
 *
 * @retval k_ra_ok / k_ra_err_invalid_arg / k_ra_err_gpio_conflict
 * @pre HAL pin validator initialized.
 * @pre SW4-4 ON (Arduino vs Octo-SPI selection).
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_arduino_pin_init(ra_board_arduino_pin_t  pin,
                                                 ra_board_arduino_mode_t mode);

/**
 * @brief Drive a previously-initialized Arduino GPIO pin.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_arduino_gpio_write(ra_board_arduino_pin_t pin, ra_level_t level);

/**
 * @brief Sample a previously-initialized Arduino GPIO pin.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_arduino_gpio_read(ra_board_arduino_pin_t pin,
                                                  ra_level_t*            out_level);

/* =============================================================================
 * 6. Pmod connectors (UM Section 5.3.3.1 + 5.3.3.2, Tables 17 + 19, p 26-27)
 * =============================================================================
 */

/** @brief Identifiers for the two Pmod sockets. */
typedef enum : uint8_t {
  k_ra_board_pmod1      = 0U, /**< Pmod1 -- J26, SCI2 simple-SPI/UART/I2C. UM Table 17 p 26. */
  k_ra_board_pmod2      = 1U, /**< Pmod2 -- J25, SPI/UART (RSPI/SCI0).     UM Table 19 p 27. */
  k_ra_board_pmod_count = 2U,
} ra_board_pmod_id_t;

/**
 * @brief Pin assignments for Pmod1 (J26) in SPI mode (UM Table 17 p 26).
 *
 * @details
 * Pmod1 uses SCI2 in "Simple SPI" mode and is selected by SW4-1=OFF
 * + SW4-2=OFF (default). Pmod1 conflicts with Octo-SPI when SPI/UART
 * is selected.
 */
typedef enum : uint16_t {
  k_ra_board_pmod1_spi_cs = (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_4),
  /**< Pmod1.1 CS  (SS2/IRQ14),  P804. UM Table 17 p 26. */ /* LEGACY-OK: SS2 is the UM pin-mux signal name */
  k_ra_board_pmod1_spi_copi =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_1), /**< Pmod1.2 COPI (MOSI2/TXD2 per UM),P801. UM Table 17 p 26. */
  k_ra_board_pmod1_spi_cipo =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_2), /**< Pmod1.3 CIPO (MISO2/RXD2 per UM),P802. UM Table 17 p 26. */
  k_ra_board_pmod1_spi_sck =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_3), /**< Pmod1.4 SCK  (SCK2),       P803. UM Table 17 p 26. */
} ra_board_pmod1_spi_pin_t;

/** @brief Pin assignments for Pmod1 (J26) in UART mode (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra_board_pmod1_uart_txd =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_1), /**< Pmod1 UART TXD (TXD2), P801. UM Table 17 p 26. */
  k_ra_board_pmod1_uart_rxd =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_2), /**< Pmod1 UART RXD (RXD2), P802. UM Table 17 p 26. */
  k_ra_board_pmod1_uart_cts =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_0), /**< Pmod1 UART CTS (CTS2), P800. UM Table 17 p 26. */
  k_ra_board_pmod1_uart_rts =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_4), /**< Pmod1 UART RTS (RTS2), P804. UM Table 17 p 26. */
} ra_board_pmod1_uart_pin_t;

/** @brief Pin assignments for Pmod1 (J26) in I2C mode (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra_board_pmod1_i2c_sda =
    (uint16_t)RA_PIN(k_ra_port_5,
                     k_ra_pin_11), /**< Pmod1 I2C SDA (SDA1), P511. UM Table 17 p 26. */
  k_ra_board_pmod1_i2c_scl =
    (uint16_t)RA_PIN(k_ra_port_5,
                     k_ra_pin_12), /**< Pmod1 I2C SCL (SCL1), P512. UM Table 17 p 26. */
} ra_board_pmod1_i2c_pin_t;

/** @brief Pin assignments for Pmod1 GPIO + IRQ side-band (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra_board_pmod1_irq =
    (uint16_t)RA_PIN(k_ra_port_0,
                     k_ra_pin_6), /**< Pmod1.7 IRQ (IRQ11-DS), P006. UM Table 17 p 26. */
  k_ra_board_pmod1_reset =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_2), /**< Pmod1.8 RESET, P402. UM Table 17 p 26. */
  k_ra_board_pmod1_gpio_a =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_12), /**< Pmod1.9 GPIO,  P412. UM Table 17 p 26. */
  k_ra_board_pmod1_gpio_b =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_13), /**< Pmod1.10 GPIO, P413. UM Table 17 p 26. */
} ra_board_pmod1_gpio_pin_t;

/**
 * @brief Pin assignments for Pmod2 (J25) in SPI mode (UM Table 19 p 27).
 *
 * @details
 * Pmod2 uses RSPI bus B for SPI (P601..P604). Both UART and SPI
 * are present; selection is by jumper E10/E14/E15/E16.
 */
typedef enum : uint16_t {
  k_ra_board_pmod2_spi_cs = (uint16_t)RA_PIN(k_ra_port_6, k_ra_pin_4),
  /**< Pmod2.1 CS   (SSLB0), P604. UM Table 19 p 27. */ /* LEGACY-OK: SSLB0 is the UM pin-mux signal name */
  k_ra_board_pmod2_spi_copi =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_3), /**< Pmod2.2 COPI (MOSIB per UM), P603. UM Table 19 p 27. */
  k_ra_board_pmod2_spi_cipo =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_2), /**< Pmod2.3 CIPO (MISOB per UM), P602. UM Table 19 p 27. */
  k_ra_board_pmod2_spi_sck =
    (uint16_t)RA_PIN(k_ra_port_6, k_ra_pin_1), /**< Pmod2.4 SCK  (RSPCKB),P601. UM Table 19 p 27. */
} ra_board_pmod2_spi_pin_t;

/** @brief Pin assignments for Pmod2 (J25) in UART mode (UM Table 19 p 27). */
typedef enum : uint16_t {
  k_ra_board_pmod2_uart_txd =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_3), /**< Pmod2 UART TXD (TXD0), P603. UM Table 19 p 27. */
  k_ra_board_pmod2_uart_rxd =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_2), /**< Pmod2 UART RXD (RXD0), P602. UM Table 19 p 27. */
  k_ra_board_pmod2_uart_cts =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_5), /**< Pmod2 UART CTS (CTS0), P605. UM Table 19 p 27. */
  k_ra_board_pmod2_uart_rts =
    (uint16_t)RA_PIN(k_ra_port_6,
                     k_ra_pin_4), /**< Pmod2 UART RTS (RTS0), P604. UM Table 19 p 27. */
} ra_board_pmod2_uart_pin_t;

/** @brief Pin assignments for Pmod2 GPIO + IRQ side-band (UM Table 19 p 27). */
typedef enum : uint16_t {
  k_ra_board_pmod2_irq =
    (uint16_t)RA_PIN(k_ra_port_0,
                     k_ra_pin_12), /**< Pmod2.7 IRQ (IRQ15),  P012. UM Table 19 p 27. */
  k_ra_board_pmod2_reset =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_10), /**< Pmod2.8 RESET, P410. UM Table 19 p 27. */
  k_ra_board_pmod2_gpio_a =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_9), /**< Pmod2.9 GPIO,  P409. UM Table 19 p 27. */
  k_ra_board_pmod2_gpio_b =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_4), /**< Pmod2.10 GPIO, P704. UM Table 19 p 27. */
} ra_board_pmod2_gpio_pin_t;

/* =============================================================================
 * 6b. MikroBUS connector (J21/J22, UM Section 5.3.5 + Table 21 p 29)
 * =============================================================================
 */

/**
 * @brief MikroBUS slot wiring on this board.
 *
 * @details
 * The EK-RA8D2 v1 carries a mikroBUS-compatible footprint at J21/J22 in
 * the center of the System Control + Ecosystem Access area
 * (UM Rev 1.01 Section 5.3.5 Figure 16 p 29). The socket is **not
 * populated** by default; the user can either solder the standard
 * mikroBUS headers onto J21/J22 to plug a MikroE Click in directly, or
 * use a Click-Shield-style breakout over the Arduino UNO header (Table
 * 20 p 28) -- both routes terminate at the same RA8D2 pads.
 *
 * For this project's physical bring-up:
 *
 *   - Pmod1 (J26) is occupied by the US159-DA16600EVZ Wi-Fi+BLE daughter
 *     card (uses SCI2 UART pins, requires SW4-1 ON + SW4-2 OFF).
 *   - Pmod2 (J25) is occupied by the Digilent PMOD MicroSD (uses RSPI-B).
 *   - The MikroBUS slot carries the MikroE LSM6DSO IMU 12 Click and is
 *     used in I2C mode only (SW4-4 ON to enable the MikroBUS pads,
 *     SW4-5 OFF to route the I2C/I3C pads to SDA1/SCL1).
 *
 * Per UM Table 21 p 29, all twelve mikroBUS signals are mapped below.
 * Software callers should only enable the subset they need (e.g. the
 * IMU demo touches just I2C).
 *
 * @par Reference:
 * EK-RA8D2 v1 UM Rev 1.01 Table 21 p 29 (mikroBUS Port Assignments).
 */
typedef enum : uint16_t {
  /** @brief J21.1 AN  -- analog input. */
  k_ra_board_mikrobus_an = (uint16_t)RA_PIN(k_ra_port_0, k_ra_pin_4),
  /** @brief J21.2 RST -- reset out to the Click. P201 also doubles as MD pin. */
  k_ra_board_mikrobus_rst = (uint16_t)RA_PIN(k_ra_port_2, k_ra_pin_1),
  /** @brief J21.3 CS   -- SPI chip-select (SSLB0, SW4-4 ON). */
  k_ra_board_mikrobus_cs = (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_3),
  /** @brief J21.4 SCK  -- SPI clock (RSPCKB, SW4-4 ON). */
  k_ra_board_mikrobus_sck = (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_2),
  /** @brief J21.5 CIPO -- Controller In, Peripheral Out (UM names this MISOB, SW4-4 ON). */ /* LEGACY-OK: MISOB is the UM pin-mux signal name */
  k_ra_board_mikrobus_cipo = (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_0),
  /** @brief J21.6 COPI -- Controller Out, Peripheral In (UM names this MOSIB, SW4-4 ON). */ /* LEGACY-OK: MOSIB is the UM pin-mux signal name */
  k_ra_board_mikrobus_copi = (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_1),
  /** @brief J22.1 PWM -- GTIOC10A. */
  k_ra_board_mikrobus_pwm = (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_10),
  /** @brief J22.2 INT -- hardware interrupt (IRQ-22). */
  k_ra_board_mikrobus_int = (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_1),
  /** @brief J22.3 RX  -- UART RX (RXD7, SW4-4 ON). */
  k_ra_board_mikrobus_rx = (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_8),
  /** @brief J22.4 TX  -- UART TX (TXD7, SW4-4 ON). */
  k_ra_board_mikrobus_tx = (uint16_t)RA_PIN(k_ra_port_8, k_ra_pin_9),
} ra_board_mikrobus_pin_t;

/**
 * @brief MikroBUS J22.5 / J22.6 I2C lines.
 *
 * @details Per UM Table 21 footnote *1, the I3C/I2C pads are switched by
 * SW4-5: **OFF = I2C** -> P511 (SDA1) / P512 (SCL1); ON = I3C -> P401
 * (SDA0) / P400 (SCL0). This project always runs in I2C mode, so the
 * symbols below resolve to SDA1/SCL1. Per UM Section 5.4.2 p 30,
 * the on-board pull-ups are not enabled by default; firmware must
 * either enable the internal pad pull-ups via PFS.PCR or rely on the
 * pull-ups inside the IIC_B controller / Click board itself.
 */
typedef enum : uint16_t {
  /** @brief J22.6 SDA (SW4-5 OFF) -> P511 SDA1. */
  k_ra_board_mikrobus_i2c_sda = (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_11),
  /** @brief J22.5 SCL (SW4-5 OFF) -> P512 SCL1. */
  k_ra_board_mikrobus_i2c_scl = (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_12),
} ra_board_mikrobus_i2c_pin_t;

/**
 * @brief I2C peripheral channel that backs the MikroBUS SDA/SCL pins.
 *
 * @details The RA8D2 group exposes a single IIC_B controller (channel 0).
 * The P511/P512 pins are its SDA1/SCL1 alternate-function pad pair (the
 * SDA0/SCL0 pads at P401/P400 are gated by SW4-5 and not used here).
 * Firmware that talks to a MikroBUS Click over I2C should call
 * ``ra_i3c_i2c_init`` with this channel.
 */
typedef enum : uint8_t {
  k_ra_board_mikrobus_iic_b_channel = 0U,
} ra_board_mikrobus_iic_channel_t;

/* =============================================================================
 * 6c. Project SW4 layout (UM Section 4.3 Table 3 p 16 + Table 18 p 26)
 * =============================================================================
 */

/**
 * @brief Required SW4 DIP-switch positions for this project's wiring.
 *
 * @details
 * The on-board SW4 8-position DIP picks which of the chip's muxed
 * peripherals the EK-RA8D2 routes to its connectors. Per UM Table 3
 * p 16, each bit governs a different routing decision, several of which
 * conflict (SW4-3 vs SW4-4 vs the Pmod1 mode bits). The layout below
 * is the one this project's wiring assumes:
 *
 * | SW4 | Position | Reason                                                 |
 * |-----|----------|--------------------------------------------------------|
 * | 1   | ON       | Pmod1 Mode-Sel-1 (with SW4-2 OFF -> UART for DA16600). |
 * | 2   | OFF      | Pmod1 Mode-Sel-2 (UART, see Table 18).                 |
 * | 3   | ON       | Octo-SPI Inactive -- frees Pmod1/Arduino/mikroBUS.     |
 * | 4   | ON       | Arduino + mikroBUS Connectors Active (IMU on mikroBUS).|
 * | 5   | OFF      | I2C Active on mikroBUS (P511/P512 SDA1/SCL1).          |
 * | 6   | OFF      | Default -- Parallel Display + MIPI Camera.             |
 * | 7   | OFF      | Default -- USBFS role toggle in mechanical OFF.        |
 * | 8   | OFF      | Default -- USBHS in Device mode.                       |
 *
 * The user can either flip the physical DIP switches to match, or call
 * ``ra_board_io_expander_apply_project_sw4_defaults`` to have the U15
 * PI4IOE5V6408 expander drive the same pattern in firmware (the
 * expander outputs override the mechanical DIP per UM Section 5.5.3
 * p 32).
 *
 * @par Reference:
 * EK-RA8D2 v1 UM Rev 1.01 Table 3 p 16 (Switch Configuration Definitions)
 * and Table 18 p 26 (Pmod 1 Switch Configuration).
 */
typedef enum : uint8_t {
  /** @brief U15 output byte that drives the SW4 layout above.
   *  Bit n = 1 -> SW4-(n+1) reads OFF; bit n = 0 -> reads ON. */
  k_ra_board_pi4ioe_output_project_default = 0xF2U,
  /** @brief U15 output byte that enables the on-board Octo-SPI flash.
   *  Value and bit polarity taken verbatim from the FSP EK-RA8D2
   *  quickstart ``board_cfg_switch_init`` (writes 0xF8 to the U15 output
   *  register): "All Output HIGH except OPMOD1/OPMOD0 mode-select and
   *  OSPI_OE_L which must be LOW." The flash output-enable OSPI_OE_L is
   *  ACTIVE-LOW, so bits 0/1/2 must be driven LOW (0xF8 = 0b1111_1000)
   *  to connect the IS25LX512M. The old 0xFE left OSPI_OE_L HIGH, so the
   *  flash stayed disconnected and JEDEC read 0x00FFFFFF (floating DQ).
   *  This is the same all-SW4-OFF state FSP expects on the bench. */
  k_ra_board_pi4ioe_output_octospi_active = 0xF8U,
} ra_board_pi4ioe_project_t;

/**
 * @brief Program the U15 I/O expander to apply the project's SW4 layout.
 *
 * @details
 * Equivalent to ``ra_board_io_expander_set_usbhs_device_mode`` but
 * writes ``k_ra_board_pi4ioe_output_project_default`` (0xF2) to the
 * expander's output register instead of 0xFF. After the write the
 * board is forced into "Pmod1 UART, Octo-SPI inactive, Arduino +
 * mikroBUS active, I2C on mikroBUS, USBHS Device" regardless of the
 * physical DIP positions.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                All five U15 register writes succeeded.
 * @retval k_ra_err_gpio_conflict P400/P401 already owned by another driver.
 * @retval k_ra_err_hw_init_failed IIC_B0 init failed.
 * @retval k_ra_err_nack          U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra_mstp_init`` has run.
 * @post P400/P401 are routed to SCL0/SDA0; IIC_B0 is initialized at
 *       100 kHz; U15.P0..P7 are configured as outputs driven to
 *       ``k_ra_board_pi4ioe_output_project_default``.
 *
 * @note Not thread-safe; call once from the boot context before any
 *       Pmod / Arduino / mikroBUS init that depends on SW4 routing.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_io_expander_apply_project_sw4_defaults(void);

/**
 * @brief Program the U15 I/O expander to activate the Octo-SPI flash.
 *
 * @details
 * Writes ``k_ra_board_pi4ioe_output_octospi_active`` (0xF8) to the U15
 * output register, driving the active-low OSPI_OE_L (plus the Pmod1
 * mode-select lines) LOW to connect the IS25LX512M Octo-SPI flash to
 * the OCTA bus, regardless of the physical DIP positions. The value and
 * polarity match the FSP EK-RA8D2 quickstart ``board_cfg_switch_init``.
 * With OSPI_OE_L left HIGH the flash is disconnected -- any
 * ``ra_xspi_flash_*`` op then sees a floating DQ and the JEDEC ID reads
 * 0x00FFFFFF. Call this once, early in main(), BEFORE
 * ``ra_board_xspi_pins_init`` / ``ra_xspi_init`` for any app that drives
 * the on-board Octo-SPI flash.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok                All U15 register writes succeeded.
 * @retval k_ra_err_busy          RIIC1 bus was held by another controller.
 * @retval k_ra_err_hw_timeout    U15 did not respond on RIIC1.
 * @retval k_ra_err_nack          U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra_mstp_init`` has run.
 * @post P512/P511 are routed to SCL1/SDA1; RIIC1 is initialized at
 *       100 kHz; U15.P0..P7 are outputs driven to
 *       ``k_ra_board_pi4ioe_output_octospi_active``; the OCTA bus pins
 *       are claimed by the Octo-SPI controller.
 *
 * @note Not thread-safe; call once from the boot context before the
 *       Octo-SPI bring-up.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_io_expander_set_octospi_active(void);

/* =============================================================================
 * 7. USB (UM Section 5.4.1 + 6.2, Tables 22 + 28, p 30 + 34)
 * =============================================================================
 */

/**
 * @brief USBHS host/device routing per UM Table 28 p 34.
 *
 * @details
 * USBHS (J7, USB Type-C). The HS PHY DM/DP differential pair
 * (USBH_P/USBH_N) is internal to the chip and not a board-routed
 * port pin. VBUS sense (USBHS_cVBUS_CON) and CC pins are PHY-side.
 * Only the bus signals listed below are exposed to user firmware.
 *
 * USB-FS (J11, USB Type-C, UM Table 22 p 30) is the host/dev port
 * for the full-speed peripheral and follows the same arrangement.
 */
typedef enum : uint16_t {
  /* USB-HS uses an MCU-driven VBUS-enable GPIO (handled by the ra_usb
   * host-class init paths, e.g. ra_usb_hhid_init / ra_usb_hcdc_init,
   * which route the pin as a peripheral output -> drives 5 V VBUS on
   * J7 for host-mode operation). Empirically verified: usb_host_*
   * apps boot through USB host init without fault on the bench
   * (alive PASS, see issue #40 for the 2026-06-02 confirmation).
   * No board-routed port-pin enum is needed here. */
  k_ra_board_usbhs_unmapped = 0U,
} ra_board_usbhs_pin_t;

/**
 * @brief Drive the U15 PI4IOE5V6408 I/O expander to select USB-HS device mode.
 *
 * @details
 * The EK-RA8D2 v1 carries a PI4IOE5V6408 8-bit I2C I/O expander at U15
 * (I2C address 0x43, EK-RA8D2 v1 UM Rev 1.01 Section 5.5.3 p 32 +
 * Section 4 p 16). U15 sits in parallel with the eight DIP switches of
 * SW4 -- when configured as outputs, U15's port pins override SW4 and
 * gate the same on-board mux that SW4 drives, including SW4-8 which
 * selects USB function on J7 (USB-HS): OFF = Device, ON = Host.
 *
 * U15 register convention (PI4IOE5V6408, see Renesas
 * ``ra-fsp-examples/ek_ra8t2/board_cfg_switch.c``):
 *  - 0x01 Device-ID  (expect 0xA0 or 0xA2)
 *  - 0x03 I/O direction      (1 = output)
 *  - 0x05 Output state       (1 = HIGH = SW4 OFF, 0 = LOW = SW4 ON)
 *  - 0x07 Output Hi-Z        (1 = Hi-Z)
 *  - 0x0D Pull-up / pull-down select
 *
 * Polarity: the FSP reference driver treats ``OFF == output bit HIGH``,
 * so SW4-8 OFF (the silk-screen "Device" position) corresponds to
 * U15.P7 = 1. We write 0xFF (all bits HIGH = all SW4 channels in their
 * default OFF position) which puts USB-HS into Device mode and leaves
 * the other muxed peripherals at their EK-RA8D2 default routing.
 *
 * Routes P400 -> SCL0 and P401 -> SDA0 (chip HUM I/O Ports +
 * EK-RA8D2 v1 UM Section 5.5.3) and brings IIC_B channel 0 up at
 * 100 kHz (PCLKA = 125 MHz, same divisor used by every other I2C
 * bring-up in this tree -- see ``tests/test_app_i2c_loopback.c``).
 *
 * @return ``ra_err_t`` Error code.
 * @retval k_ra_ok All five register writes succeeded; U15 is driving
 *                 SW4-8 = OFF (Device mode).
 * @retval k_ra_err_gpio_conflict P400/P401 already owned.
 * @retval k_ra_err_hw_init_failed IIC_B init failed.
 * @retval k_ra_err_nack U15 didn't ACK the register write.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ``ra_mstp_init`` has run.
 * @post P400/P401 are routed to SCL0/SDA0; IIC_B0 is initialized at
 *       100 kHz; U15.P0..P7 are configured as outputs driven HIGH.
 *
 * @note Not thread-safe; call once from the boot context immediately
 *       before ``ra_board_usbhs_device_init``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_io_expander_set_usbhs_device_mode(void);

/**
 * @brief Bring the chip USBHS module up in device mode (HS PHY).
 *
 * @retval k_ra_ok / k_ra_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_usbhs_device_init(void);

/**
 * @brief Bring the chip USBHS module up in host mode.
 *
 * @retval k_ra_ok / k_ra_err_not_supported (until USBHS HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_usbhs_host_init(void);

/* =============================================================================
 * 8. Camera connector J35 (UM Section 8.3, Tables 35 + 36, p 48 + 49)
 * =============================================================================
 */

/**
 * @enum ra_board_camera_pin_t
 * @brief Pins routed to the parallel-camera connector J35 (DVP mode).
 *
 * @details
 * UM Table 35 ("Camera Expansion Port Assignments in Parallel mode,
 * SW4-6 ON"). Goes into the chip CEU peripheral. P405/P406 are
 * shared with the audio CODEC -- see ``ra_board_audio_pin_t`` and
 * jumper J41. P902/PB02/PB03/PB04 are also shared with the parallel
 * graphics expansion port, so DVP camera and parallel TFT are
 * mutually exclusive.
 */
typedef enum : uint16_t {
  k_ra_board_cam_d0 =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_0), /**< CAM D0,    P400. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d1 =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_2), /**< CAM D1,    P902. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d2 =
    (uint16_t)RA_PIN(k_ra_port_4,
                     k_ra_pin_5), /**< CAM D2,    P405 (J41). EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d3 =
    (uint16_t)RA_PIN(k_ra_port_4,
                     k_ra_pin_6), /**< CAM D3,    P406 (J41). EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d4 =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_0), /**< CAM D4,    P700. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d5 =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_1), /**< CAM D5,    P701. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d6 =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_2), /**< CAM D6,    P702. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_d7 =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_3), /**< CAM D7,    P703. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_vsync =
    (uint16_t)RA_PIN(k_ra_port_11, k_ra_pin_2), /**< CAM VSYNC, PB02. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_hsync =
    (uint16_t)RA_PIN(k_ra_port_11, k_ra_pin_3), /**< CAM HSYNC, PB03. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_pclk =
    (uint16_t)RA_PIN(k_ra_port_11, k_ra_pin_4), /**< CAM PCLK,  PB04. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_xclk =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_1), /**< CAM XCLK,  P501. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_rst =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_9), /**< CAM RST,   P709. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_int =
    (uint16_t)RA_PIN(k_ra_port_0,
                     k_ra_pin_10), /**< CAM INT (IRQ-14), P010. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_i2c_sda =
    (uint16_t)RA_PIN(k_ra_port_5,
                     k_ra_pin_11), /**< CAM I2C SDA (SDA1), P511. EK-RA8D2 UM Table 35 p 48. */
  k_ra_board_cam_i2c_scl =
    (uint16_t)RA_PIN(k_ra_port_5,
                     k_ra_pin_12), /**< CAM I2C SCL (SCL1), P512. EK-RA8D2 UM Table 35 p 48. */
} ra_board_camera_pin_t;

/* =============================================================================
 * 9. Octo-SPI flash + SDRAM (UM Section 6.3 + 6.4, Tables 29 + 30, p 35 + 36)
 * =============================================================================
 */

/**
 * @brief Memory-mapped base addresses for off-chip XSPI / SDRAM.
 *
 * @details
 * Per UM Section 6.3 (Octo-SPI flash, IS25LX512M-JHLE 64 MB on the
 * Octo-SPI peripheral) and Section 6.4 (IS42S32160F-6BLI 64 MB
 * SDRAM on the SDRAMC bus). Bases are documented in the chip
 * Hardware User's Manual memory map; the board UM only specifies
 * the parts and their pin connections.
 */
typedef enum : uintptr_t {
  k_ra_board_xspi_flash_base = 0x80000000UL, /**< 64 MB IS25LX512M (chip HUM memory map). */
  k_ra_board_sdram_base =
    0x68000000UL, /**< 64 MB IS42S32160F (per linker scripts + ra_sdramc.c). */
} ra_board_extmem_base_t;

/** @brief Sizes of the off-chip memories, in bytes. */
typedef enum : uint32_t {
  k_ra_board_xspi_flash_size = 0x04000000UL, /**< 64 MiB. UM Section 6.3 p 35. */
  k_ra_board_sdram_size      = 0x04000000UL, /**< 64 MiB. UM Section 6.4 p 36. */
} ra_board_extmem_size_t;

/**
 * @enum ra_board_xspi_pin_t
 * @brief Pins routed to the on-board IS25LX512M Octo-SPI flash.
 *
 * @details
 * UM Table 29 ("Octo-SPI Flash Assignments"). Selection is gated by
 * SW4-3 (Octo-SPI vs Arduino/Pmod1). The chip-select strobe is
 * OSPI_FLASH_S_L, mapped to P104.
 */
typedef enum : uint16_t {
  k_ra_board_xspi_cs =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_4), /**< OSPI_FLASH_S_L, P104. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_clk =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_8), /**< OSPI_FLASH_C,   P808. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dqs =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_1), /**< OSPI_FLASH_DQS, P801. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_reset =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_6), /**< OSPI_FLASH_RESET_L, P106. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq0 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_0), /**< OSPI_FLASH_DQ0, P100. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq1 =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_3), /**< OSPI_FLASH_DQ1, P803. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq2 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_3), /**< OSPI_FLASH_DQ2, P103. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq3 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_1), /**< OSPI_FLASH_DQ3, P101. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq4 =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_2), /**< OSPI_FLASH_DQ4, P102. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq5 =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_0), /**< OSPI_FLASH_DQ5, P800. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq6 =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_2), /**< OSPI_FLASH_DQ6, P802. EK-RA8D2 UM Table 29 p 35. */
  k_ra_board_xspi_dq7 =
    (uint16_t)RA_PIN(k_ra_port_8,
                     k_ra_pin_4), /**< OSPI_FLASH_DQ7, P804. EK-RA8D2 UM Table 29 p 35. */
} ra_board_xspi_pin_t;

/**
 * @brief Route the 12 OCTA bus pins to the xSPI peripheral and pulse RESET_L.
 *
 * @details
 * Configures CS / CK / DQS / DQ0..DQ7 under PSEL=k_ra_psel_qspi (0x1C)
 * per EK-RA8D2 UM Table 29 (p 35), then drives the active-low RESET
 * strap as a GPIO output: low for >= tRLRH, high for >= tRHSL before
 * the first xSPI command. Must run BEFORE any `ra_xspi_*` call.
 *
 * @return ra_err_t Error code.
 * @retval k_ra_ok All pins routed and reset strap pulsed.
 * @retval k_ra_err_invalid_arg PFS programming rejected an entry.
 *
 * @pre IOPORT MSTP cleared (ra_pfs_init has run).
 * @pre Single-threaded init context.
 * @post 12 OCTA pins are under PSEL=0x1C.
 * @post RESET_L is high; the IS25LX512M is ready for its first command.
 *
 * @note Not thread-safe; init context only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_xspi_pins_init(void);

/* =============================================================================
 * 10. MIPI-DSI graphics expansion port J32
 *     (UM Section 8.2, Table 34, page 45)
 * =============================================================================
 */

/**
 * @enum ra_board_mipi_dsi_pin_t
 * @brief MIPI-DSI display-connector control pins (J32).
 *
 * @details
 * UM Table 34 ("MIPI Graphics Expansion Port Assignments"). The
 * high-speed DSI clock + 2 data lanes (MIPI_DSI_CL_P/N,
 * MIPI_DSI_DL0_P/N, MIPI_DSI_DL1_P/N) are dedicated MIPI-PHY balls
 * and are not exposed as general-purpose port pins; only the
 * low-speed control side (TE / reset / backlight / I2C touch /
 * touch INT) is BSP-addressable.
 */
typedef enum : uint16_t {
  k_ra_board_mipi_dsi_te =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_11), /**< DSI tearing-effect, P411. UM Table 34 p 45. */
  k_ra_board_mipi_dsi_reset_n =
    (uint16_t)RA_PIN(k_ra_port_6, k_ra_pin_6), /**< DSI DISP_RST, P606. UM Table 34 p 45. */
  k_ra_board_mipi_dsi_backlight =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_14), /**< DSI DISP_BLEN, P514. UM Table 34 p 45. */
  k_ra_board_mipi_dsi_touch_int =
    (uint16_t)RA_PIN(k_ra_port_1,
                     k_ra_pin_11), /**< DSI DISP_INT (IRQ-19), P111. UM Table 34 p 45. */
  k_ra_board_mipi_dsi_i2c_sda =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_11), /**< DSI I2C SDA1, P511. UM Table 34 p 45. */
  k_ra_board_mipi_dsi_i2c_scl =
    (uint16_t)RA_PIN(k_ra_port_5, k_ra_pin_12), /**< DSI I2C SCL1, P512. UM Table 34 p 45. */
} ra_board_mipi_dsi_pin_t;

/**
 * @brief Bring the MIPI-DSI link up (PHY + DSI controller).
 *
 * @retval k_ra_ok / k_ra_err_not_supported (until MIPI-DSI HAL lands)
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_mipi_dsi_init(void);

/* =============================================================================
 * 11. J-Link OB VCOM serial bridge (UM Section 5.2.4, Table 13, page 24)
 * =============================================================================
 */

/**
 * @enum ra_board_uart_t
 * @brief Identifiers for board-routed UART consoles.
 *
 * @details
 * The EK-RA8D2 v1 carries the J-Link OB debug MCU's USB-CDC virtual-COM
 * bridge on chip pins PD02 (TXD) / PD03 (RXD) per UM Table 13 p 24.
 * Hardware-flow-control lines PD04 (RTS) and PD05 (CTS) are present on
 * the same connector but are gated by trace-cut links E17 / E9 and are
 * not used by the basic console wiring.
 *
 * On the RA8D2 PD02/PD03 are the SCI8 TXD8/RXD8 alternate functions
 * (chip HUM "Multiplexed Pin Function Selector"); the BSP forwards to
 * SCI channel 8 via ``ra_sci_init`` / ``ra_sci_write_polling`` /
 * ``ra_sci_getc_polling``.
 */
typedef enum : uint8_t {
  k_ra_board_uart_console = 0U, /**< J-Link OB VCOM bridge: PD02 TXD / PD03 RXD on SCI8.
                                 *   EK-RA8D2 UM Table 13 p 24. */
} ra_board_uart_t;

/**
 * @brief SCI channel that backs ``k_ra_board_uart_console``.
 *
 * @details
 * Exposed as an enum (not a macro) so test code and applications can
 * reference the channel without re-encoding the magic number. The
 * value comes from PD02/PD03's Multiplexed Pin Function Selector
 * row in the chip HUM I/O Ports chapter (TXD8/RXD8 alternate).
 */
typedef enum : uint8_t {
  k_ra_board_uart_console_sci_channel =
    8U, /**< PD02/PD03 -> SCI8 (verified on real EK-RA8D2 v1 silicon). */
} ra_board_uart_sci_channel_t;

/**
 * @brief Pin assignments for the J-Link OB VCOM bridge (UM Table 13 p 24).
 *
 * @details
 * PD02 / PD03 are always wired to the debug MCU's CDC bridge. PD04 /
 * PD05 are the optional RTS / CTS lines (links E17 / E9, both closed
 * by default). Chip-coordinate names: P1302 / P1303 / P1304 / P1305
 * (port 13, pins 2..5).
 */
typedef enum : uint16_t {
  k_ra_board_uart_console_pin_txd =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_2), /**< PD02 TXD. UM Table 13 p 24. */
  k_ra_board_uart_console_pin_rxd =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_3), /**< PD03 RXD. UM Table 13 p 24. */
  k_ra_board_uart_console_pin_rts =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_4), /**< PD04 RTS (link E17). UM Table 13 p 24. */
  k_ra_board_uart_console_pin_cts =
    (uint16_t)RA_PIN(k_ra_port_13, k_ra_pin_5), /**< PD05 CTS (link E9).  UM Table 13 p 24. */
} ra_board_uart_console_pin_t;

/**
 * @brief Configure SCI8 + PD02/PD03 as the debug-console UART.
 *
 * @details
 * Routes PD02 -> TXD8 and PD03 -> RXD8 (PSEL = SCI async) and brings
 * SCI8 up via ``ra_sci_init`` with 8N1 framing at the requested baud.
 * The SCI module operating clock on RA8D2 is PCLKA (chip HUM Ch 38.2
 * "SCI registers"); the BRR divisor is computed from the **current**
 * PCLKA frequency reported by ``ra_cgc_get_clock_hz``, so callers must
 * call ``ra_cgc_init()`` **before** ``ra_board_uart_console_init``.
 * Applications that retune CGC afterwards must additionally call
 * ``ra_sci_set_baud((uint8_t)k_ra_board_uart_console_sci_channel,
 * baud, new_pclka_hz)`` to recompute the divisor.
 *
 * @param[in] baud Target line rate in bps (e.g. 115200).
 *
 * @retval k_ra_ok                  Console up, ready to TX/RX.
 * @retval k_ra_err_invalid_arg     baud == 0.
 * @retval k_ra_err_not_initialized ``ra_cgc_init`` has not yet published
 *                                  a usable PCLKA value (chip still on
 *                                  MOCO / pre-PLL).
 * @retval k_ra_err_gpio_conflict   PD02 or PD03 already owned.
 * @retval k_ra_err_hw_init_failed  Underlying ``ra_sci_init`` failed.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @pre ``ra_cgc_init()`` has run and PCLKA is post-PLL.
 * @pre ra_mstp_init() has run.
 * @post SCI8 is enabled with TE=RE=1; PD02/PD03 are routed to SCI8.
 * @post BRR computed against the live PCLKA value (no hardcoded clock).
 *
 * @note Not thread-safe; call once during board bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_uart_console_init(uint32_t baud);

/**
 * @brief Polled blocking write to the J-Link OB VCOM console.
 *
 * @param[in] data Bytes to transmit; must be non-NULL when ``len`` > 0.
 * @param[in] len  Number of bytes in ``data``.
 *
 * @retval k_ra_ok                  All bytes pushed to TDR.
 * @retval k_ra_err_invalid_arg     ``data`` NULL with non-zero ``len``.
 * @retval k_ra_err_not_initialized ``ra_board_uart_console_init`` not called.
 *
 * @pre ra_board_uart_console_init succeeded.
 * @post All ``len`` bytes have been handed to the SCI8 TDR shift register.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_uart_console_write(const uint8_t* data, size_t len);

/**
 * @brief Polled non-blocking read from the J-Link OB VCOM console.
 *
 * @details
 * Drains up to ``cap`` bytes from SCI8 RDR while RDRF stays set. Stops
 * (without error) the first time RDRF clears so the call never blocks
 * waiting for a host that is silent.
 *
 * @param[out] out      Destination buffer (non-NULL when ``cap`` > 0).
 * @param[in]  cap      Capacity of ``out`` in bytes.
 * @param[out] out_len  Number of bytes actually read; non-NULL.
 *
 * @retval k_ra_ok                  Read complete; *out_len in [0, cap].
 * @retval k_ra_err_invalid_arg     out / out_len NULL with non-zero cap.
 * @retval k_ra_err_not_initialized Console not initialized.
 *
 * @pre ra_board_uart_console_init succeeded.
 * @post 0 <= *out_len <= cap.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len);

/**
 * @brief Block until every byte queued on the J-Link OB VCOM console
 *        has finished clocking out on the wire.
 *
 * @details
 * Thin wrapper around ``ra_sci_flush(k_ra_board_uart_console_sci_channel)``
 * that polls CSR.TEND on the SCI8 channel that backs the J-Link OB VCOM
 * bridge (HUM Ch 38.2.17 "CSR : Common Status Register", p 2225). The
 * intended caller is a panic-handler that needs the failure log to
 * reach the host before WFI gates the SCI clock and silently drops the
 * remaining FIFO contents.
 *
 * @return ``ra_err_t`` error code.
 * @retval k_ra_ok TEND observed (or simulator stub).
 * @retval k_ra_err_not_initialized ``ra_board_uart_console_init`` not called.
 * @retval k_ra_err_hw_timeout Spin budget elapsed without TEND.
 *
 * @pre ``ra_board_uart_console_init`` succeeded.
 * @post On success, every byte previously passed to
 *       ``ra_board_uart_console_write`` has been transmitted.
 *
 * @note Not thread-safe with respect to a concurrent
 *       ``ra_board_uart_console_write`` -- the writer may refill the
 *       shift register while the flush is polling.
 *
 * @see ra_sci_flush
 * @see ra_board_uart_console_write
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_uart_console_flush(void);

/* =============================================================================
 * 12. On-board RGMII Ethernet PHY (UM Section 6.1, Tables 26 + 27, p 33-34)
 * =============================================================================
 */

/**
 * @brief Pin assignments for the on-board MaxLinear PEF7071 (GPY111) PHY.
 *
 * @details
 * Per UM Table 26 ("Ethernet Port Assignments") p 33. The PHY is wired
 * to the RA8D2 ETHERC0 / RMAC0 in **RGMII** mode (UM 6.1: "RGMII
 * Ethernet Physical Layer Transceiver"). The 25 MHz reference clock
 * is sourced from a discrete oscillator (ECS-250-10-37B-CTN-TR, UM
 * Table 27 p 34) connected directly to the PHY -- the MCU does not
 * drive REFCLK. Ethernet data rails are gated by trace-cut links
 * E18..E24, E33, E34, E36..E38 (all closed by default).
 */
typedef enum : uint16_t {
  k_ra_board_eth_pin_mdint =
    (uint16_t)RA_PIN(k_ra_port_1, k_ra_pin_7), /**< MDINT, P107. UM Table 26 p 33. */
  k_ra_board_eth_pin_mdc =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_15), /**< MDC,   P415. UM Table 26 p 33. */
  k_ra_board_eth_pin_mdio =
    (uint16_t)RA_PIN(k_ra_port_4, k_ra_pin_14), /**< MDIO,  P414. UM Table 26 p 33. */
  k_ra_board_eth_pin_txd0 =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_7), /**< TXD0,  P307 (E21). UM Table 26 p 33. */
  k_ra_board_eth_pin_txd1 =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_6), /**< TXD1,  P306 (E20). UM Table 26 p 33. */
  k_ra_board_eth_pin_txd2 =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_5), /**< TXD2,  P305 (E19). UM Table 26 p 33. */
  k_ra_board_eth_pin_txd3 =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_4), /**< TXD3,  P304 (E18). UM Table 26 p 33. */
  k_ra_board_eth_pin_tx_ctl =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_10), /**< TX_CTL, P310 (E22). UM Table 26 p 33. */
  k_ra_board_eth_pin_tx_clk =
    (uint16_t)RA_PIN(k_ra_port_3, k_ra_pin_9), /**< TX_CLK, P309 (E23). UM Table 26 p 33. */
  k_ra_board_eth_pin_rxd0 =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_6), /**< RXD0,  P906 (E36). UM Table 26 p 33. */
  k_ra_board_eth_pin_rxd1 =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_7), /**< RXD1,  P907 (E34). UM Table 26 p 33. */
  k_ra_board_eth_pin_rxd2 =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_8), /**< RXD2,  P908 (E33). UM Table 26 p 33. */
  k_ra_board_eth_pin_rxd3 =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_9), /**< RXD3,  P909 (E24). UM Table 26 p 33. */
  k_ra_board_eth_pin_rx_ctl =
    (uint16_t)RA_PIN(k_ra_port_2, k_ra_pin_6), /**< RX_CTL, P206 (E38). UM Table 26 p 33. */
  k_ra_board_eth_pin_rx_clk =
    (uint16_t)RA_PIN(k_ra_port_9, k_ra_pin_5), /**< RX_CLK, P905 (E37). UM Table 26 p 33. */
  k_ra_board_eth_pin_rstn =
    (uint16_t)RA_PIN(k_ra_port_7, k_ra_pin_8), /**< RSTN,   P708.        UM Table 26 p 33. */
} ra_board_eth_pin_t;

/**
 * @brief ETHA / RMAC port and PHY MDIO address for the on-board PHY.
 *
 * @details
 * Per UM 6.1 ("Ethernet interface") and the canonical FSP example
 * project ``ethernet_ek_ra8d2_ep`` (ra_cfg.txt: "Channel: 1" for both
 * g_ether0 and g_rmac_phy0). The on-board PHY (MaxLinear PEF7071VV16-
 * LLHU, UM Table 27 p 34) is the only device on the MDIO bus and
 * powers up at MDIO address 0; the RA8D2 has two ETHA / RMAC
 * instances, of which **port 1** (not port 0) is the one wired to the
 * RJ45 J33 on the EK-RA8D2. Earlier revisions of this header pointed
 * at port 0 -- that was wrong; the chip-side MDIO controller never saw a
 * reply because the MDC / MDIO pads on RMAC0 are not routed to the
 * board's PHY.
 */
typedef enum : uint8_t {
  k_ra_board_eth_etha_port = 1U, /**< ETHA1 (k_ra_etha_port_1). UM 6.1.   */
  k_ra_board_eth_rmac_port = 1U, /**< RMAC1 (k_ra_rmac_port_1). UM 6.1.   */
  k_ra_board_eth_phy_addr  = 0U, /**< MDIO addr of the on-board PHY (HW
                                  *   strap on PEF7071, UM Table 27 p 34). */
} ra_board_eth_index_t;

/**
 * @brief Bring up the on-board RGMII Ethernet PHY (PEF7071) and RMAC0.
 *
 * @details
 * 1. Routes all sixteen Ethernet data / control pins (UM Table 26 p 33)
 *    to their ETHERC RGMII alternate functions via
 *    ``ra_pfs_route_peripheral`` (PSEL = ``k_ra_psel_ether_rmii`` --
 *    same PSEL slot covers RMII and RGMII on RA8D2; the per-pin mux
 *    is identical and the ESWM MIICR1.MIISEL field picks RGMII).
 * 2. Initialises ETHA0 with the default ``ra_etha_config_t`` (RESET
 *    mode, no IRQs enabled) via ``ra_etha_init``.
 * 3. Initialises RMAC0 with ``phy_interface = k_ra_rmac_pis_gmii``,
 *    ``link_speed = k_ra_rmac_lsc_1000mbit``, ``duplex = full`` via
 *    ``ra_rmac_init`` (auto-negotiation overrides this once link
 *    comes up; RMAC.MPIC.PIS is GMII for 1 Gbps and MII for
 *    10/100 Mbps per HUM Table 29.11).
 *
 * The on-board PHY's 25 MHz reference is provided by an external
 * crystal oscillator (UM Table 27 p 34); no chip-side REFCLK output
 * programming is needed. Caller is responsible for driving RSTN
 * (P708) low for >= 10 us before this function and for starting
 * auto-negotiation via ``ra_rmac_phy_auto_neg_start`` after it
 * returns.
 *
 * @retval k_ra_ok                  PHY pins routed; ETHA0 + RMAC0 up.
 * @retval k_ra_err_gpio_conflict   At least one Ethernet pin is owned.
 * @retval k_ra_err_hw_init_failed  ETHA or RMAC init failed.
 *
 * @pre IOPORT module powered (reset default).
 * @pre ra_mstp_init() has run.
 * @post All sixteen Ethernet pins are in RGMII alternate-function mode.
 * @post ETHA0 and RMAC0 are initialized and ready for descriptor-ring
 *       configuration / auto-negotiation.
 *
 * @note Not thread-safe; call once during board bring-up.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_board_ethernet_init(void);

#ifdef __cplusplus
}
#endif
