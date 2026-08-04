/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_board_ek_ra8d2_connectors.h
 * @brief Board-identity, LEDs, switches, display, audio, Arduino, Pmod, and
 *        MikroBUS portion of the EK-RA8D2 v1 board-support layer.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sub-header of ``ra8_board_ek_ra8d2.h`` (the thin umbrella). Carries the
 * board-identity strings + struct, the user LEDs (Section 1), user
 * switches (Section 2), the parallel-RGB graphics expansion connector J1
 * (Section 3), the DA7212 audio CODEC (Section 4), the Arduino Uno header
 * (Section 5), the Pmod1/Pmod2 sockets (Section 6), and the MikroBUS slot
 * (Section 6b) together with the project SW4 DIP layout enum (Section 6c).
 *
 * Every declaration here was moved VERBATIM out of ``ra8_board_ek_ra8d2.h``;
 * no contract, Doxygen block, or HUM/UM citation has changed. Consumers
 * keep including ``ra8_board_ek_ra8d2.h``; this file is pulled in by that
 * umbrella and should not be included directly.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * @since 0.1.0
 */

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "ra8_err.h"
#include "ra8_port_constants.h"

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
 * Returned by ``ra8_board_get_info()`` so applications can log /
 * verify the board they were built for. All three are ASCII string
 * literals with permanent storage duration.
 */
extern const char* const k_ra8_board_name;    /**< "EK-RA8D2 v1".              */
extern const char* const k_ra8_board_doc_rev; /**< "R20UT5523EG0101 Rev 1.01". */
extern const char* const k_ra8_board_mcu;     /**< "R7KA8D2KFLCAC".            */

/**
 * @struct ra8_board_info_t
 * @brief Snapshot of board identity strings.
 */
typedef struct {
  const char* name;    /**< Same value as ``k_ra8_board_name``.    */
  const char* doc_rev; /**< Same value as ``k_ra8_board_doc_rev``. */
  const char* mcu;     /**< Same value as ``k_ra8_board_mcu``.     */
} ra8_board_info_t;

/**
 * @brief Copy the three board-identity strings into ``out``.
 *
 * @param[out] out Destination struct, must be non-NULL.
 *
 * @retval k_ra8_ok                 Filled.
 * @retval k_ra8_err_invalid_arg    out == NULL.
 *
 * @pre out is writable.
 * @post On success out->name / doc_rev / mcu point at the global
 *       ``k_ra8_board_*`` strings.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_get_info(ra8_board_info_t* out);

/* =============================================================================
 * 1. User LEDs (UM Section 5.5.1, Table 24, page 31)
 * =============================================================================
 */

/**
 * @enum ra8_board_led_id_t
 * @brief Identifiers for the three user LEDs on EK-RA8D2 v1.
 *
 * @details
 * Pins per UM Table 24 ("EK-RA8D2 Board LED Functions"). Trace-cut
 * jumpers E27/E26/E28 (closed by default) tie the LEDs to P600/P303/PA07
 * respectively. All three LEDs are active-high.
 */
typedef enum : uint8_t {
  k_ra8_board_led1 = 0U, /**< LED1, BLUE,  P600 (jumper E27). EK-RA8D2 UM Table 24 p 31. */
  k_ra8_board_led2 = 1U, /**< LED2, GREEN, P303 (jumper E26). EK-RA8D2 UM Table 24 p 31. */
  k_ra8_board_led3 = 2U, /**< LED3, RED,   PA07 (jumper E28). EK-RA8D2 UM Table 24 p 31. */

  /** Convenience aliases by colour. */
  k_ra8_board_led_blue  = k_ra8_board_led1,
  k_ra8_board_led_green = k_ra8_board_led2, /**< RA8 board led green. */
  k_ra8_board_led_red   = k_ra8_board_led3, /**< RA8 board led red.   */

  k_ra8_board_led_count = 3U, /**< RA8 board led count. */
} ra8_board_led_id_t;

/**
 * @brief Configure ``led`` as a digital output, initial level low (off).
 *
 * @retval k_ra8_ok                  Pin claimed and driven low.
 * @retval k_ra8_err_invalid_arg     led >= k_ra8_board_led_count.
 * @retval k_ra8_err_gpio_conflict   Pin already owned.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @post LED is off and pin is configured as a digital output.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_init(ra8_board_led_id_t led);

/**
 * @brief Drive ``led`` HIGH (light it).
 * @retval k_ra8_ok / k_ra8_err_invalid_arg
 * @pre ra8_board_led_init(led) succeeded.
 * @post LED pin output latch == 1.
 */
[[nodiscard]] ra8_err_t ra8_board_led_on(ra8_board_led_id_t led);

/**
 * @brief Drive ``led`` LOW (extinguish it).
 * @retval k_ra8_ok / k_ra8_err_invalid_arg
 * @pre ra8_board_led_init(led) succeeded.
 * @post LED pin output latch == 0.
 */
[[nodiscard]] ra8_err_t ra8_board_led_off(ra8_board_led_id_t led);

/**
 * @brief Toggle ``led``'s output state.
 * @retval k_ra8_ok / k_ra8_err_invalid_arg
 * @pre ra8_board_led_init(led) succeeded.
 * @post LED pin output latch is inverted from its prior value.
 */
[[nodiscard]] ra8_err_t ra8_board_led_toggle(ra8_board_led_id_t led);

/**
 * @brief Translate a board LED id into its underlying ``ra8_port_pin_t``.
 *
 * @param[in]  led      LED identifier.
 * @param[out] out_pin  Destination pin id.
 * @retval k_ra8_ok / k_ra8_err_invalid_arg
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_pin(ra8_board_led_id_t led, ra8_port_pin_t* out_pin);

/* =============================================================================
 * 2. User switches (UM Section 5.5.2, Table 25, page 32)
 * =============================================================================
 */

/**
 * @enum ra8_board_sw_id_t
 * @brief Identifiers for the on-board user push-buttons.
 *
 * @details
 * Pins per UM Table 25 ("EK-RA8D2 Board Switches"). SW3 is tied to
 * the chip RESET_L line and is therefore not exposed as a
 * software-readable input here.
 */
typedef enum : uint8_t {
  k_ra8_board_sw1      = 0U, /**< SW1, P009, IRQ13-DS (jumper E31). EK-RA8D2 UM Table 25 p 32. */
  k_ra8_board_sw2      = 1U, /**< SW2, P008, IRQ12-DS (jumper E32). EK-RA8D2 UM Table 25 p 32. */
  k_ra8_board_sw_count = 2U, /**< RA8 board sw count.                                          */
} ra8_board_sw_id_t;

/**
 * @brief Per-switch IRQ channel numbers (UM Table 25 p 32).
 *
 * @details
 * SW1 -> IRQ13-DS, SW2 -> IRQ12-DS. The "-DS" suffix means
 * deep-sleep wake capable. Values are bare IRQ channel numbers
 * usable with ``ra8_icu_configure_irq_pin``.
 */
typedef enum : uint8_t {
  k_ra8_board_sw1_irq = 13U, /**< SW1 -> IRQ13-DS. UM Table 25 p 32. */
  k_ra8_board_sw2_irq = 12U, /**< SW2 -> IRQ12-DS. UM Table 25 p 32. */
} ra8_board_sw_irq_t;

/** @brief Logical "pressed" / "released" state. */
typedef enum : uint8_t {
  k_ra8_board_sw_released = 0U, /**< RA8 board sw released. */
  k_ra8_board_sw_pressed  = 1U, /**< RA8 board sw pressed.  */
} ra8_board_sw_state_t;

/**
 * @brief Configure a switch pin as an input with internal pull-up.
 *
 * @retval k_ra8_ok / k_ra8_err_invalid_arg / k_ra8_err_gpio_conflict
 * @pre HAL pin validator initialized.
 * @post Pin is digital input, pull-up enabled.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_init(ra8_board_sw_id_t sw);

/**
 * @brief Sample the current state of ``sw``.
 *
 * @details
 * The buttons are wired active-low (press shorts the pin to GND);
 * this function inverts the level so callers receive a positive
 * "pressed = true" semantic.
 *
 * @param[in]  sw           Switch id.
 * @param[out] out_pressed  Set to ``k_ra8_board_sw_pressed`` if held.
 *
 * @retval k_ra8_ok                 Read complete.
 * @retval k_ra8_err_invalid_arg    sw out of range or out_pressed NULL.
 *
 * @pre ra8_board_sw_init(sw) succeeded.
 * @post *out_pressed is one of {released, pressed}.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_read(ra8_board_sw_id_t sw, ra8_board_sw_state_t* out_pressed);

/** @brief Switch IRQ callback signature. */
typedef void (*ra8_board_sw_irq_cb_t)(void* ctx);

/**
 * @brief Wire ``sw`` to the ICU and register a falling-edge callback.
 *
 * @details
 * Buttons are active-low so a falling edge corresponds to a press.
 * The HAL takes care of the PFS routing, IRQCR programming, and
 * NVIC enable; this veneer just hides the specific channel number
 * (UM Table 25) from the caller.
 *
 * @retval k_ra8_ok / k_ra8_err_invalid_arg / k_ra8_err_gpio_conflict
 * @pre ra8_icu_init() has been called once during boot.
 * @post Pressing ``sw`` invokes ``cb(ctx)`` from the ICU ISR context.
 */
[[nodiscard]] ra8_err_t
ra8_board_sw_attach_irq(ra8_board_sw_id_t sw, ra8_board_sw_irq_cb_t cb, void* ctx);

/**
 * @brief Translate a board switch id into its underlying ``ra8_port_pin_t``.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_pin(ra8_board_sw_id_t sw, ra8_port_pin_t* out_pin);

/* =============================================================================
 * 3. Parallel-RGB Graphics Expansion connector J1
 *    (UM Section 8.1, Table 33, page 42)
 * =============================================================================
 */

/** @brief GLCDC parallel-RGB pixel formats supported by EK-RA8D2 J1. */
typedef enum : uint8_t {
  k_ra8_board_glcdc_fmt_rgb888 = 0U, /**< 24-bit, 8 bits/colour. */
  k_ra8_board_glcdc_fmt_rgb666 = 1U, /**< 18-bit, 6 bits/colour. */
  k_ra8_board_glcdc_fmt_rgb565 = 2U, /**< 16-bit, 5/6/5 bits.    */
} ra8_board_glcdc_fmt_t;

/** @brief One row of the GLCDC pin table -- (signal name, RA8D2 pin). */
typedef struct {
  const char*    signal; /**< Human-readable signal name (UM Table 33). */
  ra8_port_pin_t pin;    /**< RA8D2 port pin carrying that signal.      */
} ra8_board_glcdc_pin_t;

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
extern const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb888_pins[];
extern const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb666_pins[];
extern const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb565_pins[];

/** @brief Number of entries in each per-format pin table. */
extern const uint32_t g_ra8_board_glcdc_rgb888_pin_count;
extern const uint32_t g_ra8_board_glcdc_rgb666_pin_count;
extern const uint32_t g_ra8_board_glcdc_rgb565_pin_count;

/**
 * @brief Program every J1 pin for ``fmt`` to its GLCDC alternate function.
 *
 * @details
 * Walks the right table above and calls ``ra8_pfs_route_peripheral``
 * for each entry. Does NOT bring the GLCDC peripheral itself up --
 * call ``ra8_glcdc_init`` after this returns.
 *
 * @retval k_ra8_ok                 All pins routed.
 * @retval k_ra8_err_invalid_arg    fmt out of range.
 * @retval k_ra8_err_gpio_conflict  At least one pin already owned.
 *
 * @pre IOPORT module powered (true at reset).
 * @post Every J1 pin for the chosen format is in GLCDC mode.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_glcdc_init(ra8_board_glcdc_fmt_t fmt);

/** @brief Board-style aliases for the J1 LCD GPIO control pins.
 *
 *  The underlying ``ra8_port_pin_t`` enum members live in
 *  ``libs/ra8_core/inc/ra8_port_constants.h`` (the same place LED1/2/3
 *  are declared) so casting them to ``ra8_port_pin_t`` is type-safe
 *  for the analyzer.  Source: EK-RA8D2 v1 UM Table 33 p 42.
 */
typedef enum : uint16_t {
  k_ra8_board_lcd_reset_l = (uint16_t)k_ra8_pin_lcd_reset_l, /**< J1-6 RST,  P606 (active-low).  */
  k_ra8_board_lcd_blen    = (uint16_t)k_ra8_pin_lcd_blen,    /**< J1-1 BLEN, P514 (active-high). */
} ra8_board_lcd_gpio_pin_t;

/**
 * @brief Pulse the J1 panel's RESET_L line and assert BLEN backlight.
 *
 * @details Drives `k_ra8_board_lcd_reset_l` low for 50 ms, releases it
 * high, waits another 50 ms for the panel's internal controller to
 * latch reset-release, then drives `k_ra8_board_lcd_blen` high to
 * enable the backlight.  Must be called BEFORE `ra8_board_glcdc_init`
 * routes the data pins to PSEL=glcdc so the panel sees stable signals
 * by the time GLCDC starts driving them.
 *
 * @retval k_ra8_ok               Panel reset released and backlight on.
 * @retval k_ra8_err_invalid_arg  GPIO init for one of the pins failed.
 * @retval k_ra8_err_gpio_*       Underlying ra8_gpio_* propagated error.
 *
 * @pre IOPORT module is powered (true at reset).
 * @pre `ra8_time_init` has been called (function blocks on `ra8_delay_ms`).
 * @post P606 (RESET_L) is driven high; panel is out of reset.
 * @post P514 (BLEN) is driven high; backlight is on.
 *
 * @note Single-threaded init context; blocks for ~100 ms total.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_lcd_panel_power_on(void);

/**
 * @brief Turn the J1 panel backlight (BLEN, P514) on or off.
 *
 * @details Drives the active-high `k_ra8_board_lcd_blen` output high (`on ==
 * true`) or low (`on == false`). BLEN is configured as a GPIO output by
 * ::ra8_board_lcd_panel_power_on, so this is a level change only -- no
 * re-init. Intended for an application idle-dim / auto-off policy: the LED
 * backlight is the single largest load on a battery-powered backlit TFT, so
 * blanking it while the reader sits idle on a static page is the highest-value
 * power saving available. Brightness (PWM) control is a separate capability
 * (BLEN is not routed to a GPT output on this board); this is on/off only.
 *
 * @param[in] on true to light the backlight, false to blank it.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Backlight level updated.
 * @retval k_ra8_err_invalid_arg  BLEN pin invalid (should not occur).
 * @retval k_ra8_err_gpio_*       Underlying ra8_gpio_write propagated error.
 *
 * @pre ::ra8_board_lcd_panel_power_on has run (BLEN is a GPIO output).
 * @pre IOPORT module is powered (true at reset).
 * @post P514 (BLEN) is driven to the level selected by @p on.
 * @post No other pin or panel state is changed.
 *
 * @note Not thread-safe; call from the single input/render context.
 * @see ra8_board_lcd_panel_power_on
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_backlight_set(bool on);

/* =============================================================================
 * 4. Audio CODEC (DA7212 U14, SSIE + I2C)
 *    (UM Section 6.6, Table 32, page 38)
 * =============================================================================
 */

/**
 * @enum ra8_board_audio_pin_t
 * @brief Pin assignments for the on-board DA7212 audio CODEC.
 *
 * @details
 * Per UM Table 32 ("Audio CODEC Port Pin Assignments"). P405/P406
 * are shared with the parallel-camera connector via jumper J41;
 * the camera and CODEC cannot be used simultaneously.
 */
typedef enum : uint16_t {
  k_ra8_board_audio_pin_bclk =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_3), /**< P403, EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_wclk =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_4), /**< P404, EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_datin =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_5), /**< P405 (J41), EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_datout =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_6), /**< P406 (J41), EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_mclk =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_6), /**< PD06, EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_i2c_sda =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11), /**< P511 (SDA1), EK-RA8D2 UM Table 32 p 38. */
  k_ra8_board_audio_pin_i2c_scl =
    (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12), /**< P512 (SCL1), EK-RA8D2 UM Table 32 p 38. */
} ra8_board_audio_pin_t;

/**
 * @brief SSIE channel the CODEC is wired to.
 *
 * @details
 * P403..P406 carry SSIE0 signals on the RA8D2 (see chip HUM I/O Ports
 * chapter for PSEL values). Channel selection itself is not in the
 * EK-RA8D2 board manual, only the chip pins.
 */
typedef enum : uint8_t {
  k_ra8_board_audio_ssie_channel = 0U, /**< RA8 board audio ssie channel. */
} ra8_board_audio_ch_t;

/**
 * @brief Route the CODEC pins and bring up SSIE0 in I2S controller mode.
 *
 * @retval k_ra8_ok / k_ra8_err_invalid_arg / k_ra8_err_gpio_conflict
 *
 * @pre J41 jumpers populated (CODEC selected over camera).
 * @post All CODEC pins are routed; SSIE0 is enabled.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_board_audio_init(uint32_t sample_rate_hz, uint8_t bit_depth, uint8_t channels);

/**
 * @brief Push one block of stereo PCM samples to the CODEC's DAC.
 *
 * @param[in] buf  Sample buffer (interleaved L/R, 16-bit).
 * @param[in] len  Number of samples (must be even).
 *
 * @retval k_ra8_ok                  Block enqueued or transmitted.
 * @retval k_ra8_err_invalid_arg     buf NULL or len odd / zero.
 * @retval k_ra8_err_not_initialized ra8_board_audio_init not called.
 *
 * @pre ra8_board_audio_init succeeded.
 * @post Sample block has been handed to SSIE0.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_audio_play_sample_block(const int16_t* buf, uint32_t len);

/* =============================================================================
 * 5. Arduino Uno header (J18/J19/J23/J24) (UM Section 5.3.4, Table 20, p 27-28)
 * =============================================================================
 */

/**
 * @enum ra8_board_arduino_pin_t
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
  k_ra8_board_arduino_d0 =
    (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_8), /**< D0 RXD7/GTIOC13B, P808. UM Table 20 p 28. */
  k_ra8_board_arduino_d1 =
    (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_9), /**< D1 TXD7,           P809. UM Table 20 p 28. */
  k_ra8_board_arduino_d2 =
    (uint16_t)RA8_PIN(k_ra8_port_0,
                      k_ra8_pin_11), /**< D2 INT0/IRQ16,     P011. UM Table 20 p 28. */
  k_ra8_board_arduino_d3 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_11), /**< D3 INT1/GTIOC10B,  P811. UM Table 20 p 28. */
  k_ra8_board_arduino_d4 =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_10), /**< D4 GTIOC10A/IRQ21, P810. UM Table 20 p 28. */
  k_ra8_board_arduino_d5 =
    (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_4), /**< D5 GTIOC1B/IRQ1,   P104. UM Table 20 p 28. */
  k_ra8_board_arduino_d6 =
    (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_5), /**< D6 GTIOC1A/IRQ0,   P105. UM Table 20 p 28. */
  k_ra8_board_arduino_d7 =
    (uint16_t)RA8_PIN(k_ra8_port_3,
                      k_ra8_pin_12), /**< D7 IRQ22-DS,       P312. UM Table 20 p 28. */
  k_ra8_board_arduino_d8 =
    (uint16_t)RA8_PIN(k_ra8_port_13,
                      k_ra8_pin_1), /**< D8 IRQ22,          PD01. UM Table 20 p 28. */
  k_ra8_board_arduino_d9 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_10), /**< D9 GTIOC9B/IRQ20,  P110. UM Table 20 p 28. */
  k_ra8_board_arduino_d10 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_3), /**< D10 SPI_SS / GTIOC2A,  P103. UM Table 20 p 28. */
  k_ra8_board_arduino_d11 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_1), /**< D11 SPI_MOSI / GTIOC8A,P101. UM Table 20 p 28. */
  k_ra8_board_arduino_d12 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_0), /**< D12 SPI_MISO / GTIOC8B,P100. UM Table 20 p 28. */
  k_ra8_board_arduino_d13 =
    (uint16_t)RA8_PIN(k_ra8_port_1,
                      k_ra8_pin_2), /**< D13 SPI_SCK / GTIOC2B, P102. UM Table 20 p 28. */
  k_ra8_board_arduino_d14 = (uint16_t)RA8_PIN(
    k_ra8_port_5,
    k_ra8_pin_11), /**< D14 SDA1 (or SDA0=P401 if SW4-5 ON), P511. UM Table 20 p 28. */
  k_ra8_board_arduino_d15 = (uint16_t)RA8_PIN(
    k_ra8_port_5,
    k_ra8_pin_12), /**< D15 SCL1 (or SCL0=P400 if SW4-5 ON), P512. UM Table 20 p 28. */

  /* Analog input block (J19). */
  k_ra8_board_arduino_a0 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_1), /**< A0 AN001, P001. UM Table 20 p 28. */
  k_ra8_board_arduino_a1 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_7), /**< A1 AN007, P007. UM Table 20 p 28. */
  k_ra8_board_arduino_a2 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_3), /**< A2 AN003, P003. UM Table 20 p 28. */
  k_ra8_board_arduino_a3 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_4), /**< A3 AN004, P004. UM Table 20 p 28. */
  k_ra8_board_arduino_a4 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_14), /**< A4 AN014/DA0, P014. UM Table 20 p 28. */
  k_ra8_board_arduino_a5 =
    (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_15), /**< A5 AN015/DA1, P015. UM Table 20 p 28. */
} ra8_board_arduino_pin_t;

/** @brief Direction mode for an Arduino-header GPIO pin. */
typedef enum : uint8_t {
  k_ra8_board_arduino_mode_input        = 0U, /**< RA8 board arduino mode input.        */
  k_ra8_board_arduino_mode_input_pullup = 1U, /**< RA8 board arduino mode input pullup. */
  k_ra8_board_arduino_mode_output       = 2U, /**< RA8 board arduino mode output.       */
} ra8_board_arduino_mode_t;

/**
 * @brief Configure an Arduino header pin in GPIO ``mode``.
 *
 * @retval k_ra8_ok / k_ra8_err_invalid_arg / k_ra8_err_gpio_conflict
 * @pre HAL pin validator initialized.
 * @pre SW4-4 ON (Arduino vs Octo-SPI selection).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_arduino_pin_init(ra8_board_arduino_pin_t  pin,
                                                   ra8_board_arduino_mode_t mode);

/**
 * @brief Drive a previously-initialized Arduino GPIO pin.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_arduino_gpio_write(ra8_board_arduino_pin_t pin,
                                                     ra8_level_t             level);

/**
 * @brief Sample a previously-initialized Arduino GPIO pin.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_arduino_gpio_read(ra8_board_arduino_pin_t pin,
                                                    ra8_level_t*            out_level);

/* =============================================================================
 * 6. Pmod connectors (UM Section 5.3.3.1 + 5.3.3.2, Tables 17 + 19, p 26-27)
 * =============================================================================
 */

/** @brief Identifiers for the two Pmod sockets. */
typedef enum : uint8_t {
  k_ra8_board_pmod1      = 0U, /**< Pmod1 -- J26, SCI2 simple-SPI/UART/I2C. UM Table 17 p 26. */
  k_ra8_board_pmod2      = 1U, /**< Pmod2 -- J25, SPI/UART (RSPI/SCI0).     UM Table 19 p 27. */
  k_ra8_board_pmod_count = 2U, /**< RA8 board pmod count.                                     */
} ra8_board_pmod_id_t;

/**
 * @brief Pin assignments for Pmod1 (J26) in SPI mode (UM Table 17 p 26).
 *
 * @details
 * Pmod1 uses SCI2 in "Simple SPI" mode and is selected by SW4-1=OFF
 * + SW4-2=OFF (default). Pmod1 conflicts with Octo-SPI when SPI/UART
 * is selected.
 */
typedef enum : uint16_t {
  k_ra8_board_pmod1_spi_cs = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_4),
  /**< Pmod1.1 CS  (SS2/IRQ14),  P804. UM Table 17 p 26. */ /* LEGACY-OK: SS2 = UM pin-mux name */
  k_ra8_board_pmod1_spi_copi =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_1), /**< Pmod1.2 COPI (MOSI2/TXD2 per UM),P801. UM Table 17 p 26. */
  k_ra8_board_pmod1_spi_cipo =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_2), /**< Pmod1.3 CIPO (MISO2/RXD2 per UM),P802. UM Table 17 p 26. */
  k_ra8_board_pmod1_spi_sck =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_3), /**< Pmod1.4 SCK  (SCK2),       P803. UM Table 17 p 26. */
} ra8_board_pmod1_spi_pin_t;

/** @brief Pin assignments for Pmod1 (J26) in UART mode (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra8_board_pmod1_uart_txd =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_1), /**< Pmod1 UART TXD (TXD2), P801. UM Table 17 p 26. */
  k_ra8_board_pmod1_uart_rxd =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_2), /**< Pmod1 UART RXD (RXD2), P802. UM Table 17 p 26. */
  k_ra8_board_pmod1_uart_cts =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_0), /**< Pmod1 UART CTS (CTS2), P800. UM Table 17 p 26. */
  k_ra8_board_pmod1_uart_rts =
    (uint16_t)RA8_PIN(k_ra8_port_8,
                      k_ra8_pin_4), /**< Pmod1 UART RTS (RTS2), P804. UM Table 17 p 26. */
} ra8_board_pmod1_uart_pin_t;

/** @brief Pin assignments for Pmod1 (J26) in I2C mode (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra8_board_pmod1_i2c_sda =
    (uint16_t)RA8_PIN(k_ra8_port_5,
                      k_ra8_pin_11), /**< Pmod1 I2C SDA (SDA1), P511. UM Table 17 p 26. */
  k_ra8_board_pmod1_i2c_scl =
    (uint16_t)RA8_PIN(k_ra8_port_5,
                      k_ra8_pin_12), /**< Pmod1 I2C SCL (SCL1), P512. UM Table 17 p 26. */
} ra8_board_pmod1_i2c_pin_t;

/** @brief Pin assignments for Pmod1 GPIO + IRQ side-band (UM Table 17 p 26). */
typedef enum : uint16_t {
  k_ra8_board_pmod1_irq =
    (uint16_t)RA8_PIN(k_ra8_port_0,
                      k_ra8_pin_6), /**< Pmod1.7 IRQ (IRQ11-DS), P006. UM Table 17 p 26. */
  k_ra8_board_pmod1_reset =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_2), /**< Pmod1.8 RESET, P402. UM Table 17 p 26. */
  k_ra8_board_pmod1_gpio_a =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_12), /**< Pmod1.9 GPIO,  P412. UM Table 17 p 26. */
  k_ra8_board_pmod1_gpio_b =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_13), /**< Pmod1.10 GPIO, P413. UM Table 17 p 26. */
} ra8_board_pmod1_gpio_pin_t;

/**
 * @brief SCI channels that back the two Pmod sockets in Simple-SPI mode.
 *
 * @details
 * Both Pmod sockets are driven by an SCI channel in Simple-SPI mode rather
 * than by the RSPI/SPI_B peripheral, so firmware opens them with
 * ``ra8_sci_spi_init`` on the channel named here. Pmod1 (J26) sits on SCI2
 * via P801..P804 (UM Table 17 p 26); Pmod2 (J25) sits on SCI0 via
 * P601..P604 (UM Table 19 p 27). Both channel indices are a fixed board
 * fact of the EK-RA8D2 routing, which is why they live here rather than in
 * each application.
 *
 * @invariant Both values are valid ``ra8_sci_spi`` channel indices (0..9).
 *
 * @par Example:
 * @code
 * const ra8_sci_spi_cfg_t cfg = { .baud_hz = 1000000U, .pclk_hz = pclka_hz,
 *                                 .mode = k_ra8_spi_mode_3, .lsb_first = false };
 * (void)ra8_sci_spi_init((uint8_t)k_ra8_board_pmod1_sci_channel, &cfg);
 * @endcode
 *
 * @see ra8_board_pmod1_spi_pin_t
 * @see ra8_board_pmod2_spi_pin_t
 */
typedef enum : uint8_t {
  k_ra8_board_pmod1_sci_channel = 2U, /**< Pmod1 (J26) Simple-SPI is SCI2. */
  k_ra8_board_pmod2_sci_channel = 0U, /**< Pmod2 (J25) Simple-SPI is SCI0. */
} ra8_board_pmod_sci_channel_t;

/**
 * @brief Pin assignments for Pmod2 (J25) in SPI mode (UM Table 19 p 27).
 *
 * @details
 * Pmod2 SPI on P601..P604 is **SCI0 in Simple-SPI mode**, not the
 * RSPI/SPI_B peripheral: HUM Table 20.13 (PORT6) routes these pins at
 * ``PSEL = 00100b`` (k_ra8_psel_sci_async) to SCK0_B / CIPO0_B (MISO0_B) /
 * COPI0_B (MOSI0_B) / SS0_B -- the SPI (``PSEL = 00110b``) function has no
 * mapping on P601..P604 (RSPCKB/MOSIB/MISOB live on P609..P611). The EK UM
 * Table 19 labels them RSPCKB/MOSIB/MISOB after the bus role, but the silicon
 * drives them from SCI0; use ::ra8_sci_spi (channel 0) as the controller.
 * Both UART and Simple-SPI share the SCI function; jumpers E10/E14/E15/E16
 * select SPI vs UART signalling on pins 1/4.
 */
typedef enum : uint16_t {
  k_ra8_board_pmod2_spi_cs = (uint16_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_4),
  /**< Pmod2.1 CS   (SS0_B),  P604. UM Table 19 p 27. */
  k_ra8_board_pmod2_spi_copi =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_3), /**< Pmod2.2 COPI (COPI0_B), P603. UM Table 19 p 27. */
  k_ra8_board_pmod2_spi_cipo =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_2), /**< Pmod2.3 CIPO (CIPO0_B), P602. UM Table 19 p 27. */
  k_ra8_board_pmod2_spi_sck =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_1), /**< Pmod2.4 SCK  (SCK0_B),  P601. UM Table 19 p 27. */
} ra8_board_pmod2_spi_pin_t;

/** @brief Pin assignments for Pmod2 (J25) in UART mode (UM Table 19 p 27). */
typedef enum : uint16_t {
  k_ra8_board_pmod2_uart_txd =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_3), /**< Pmod2 UART TXD (TXD0), P603. UM Table 19 p 27. */
  k_ra8_board_pmod2_uart_rxd =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_2), /**< Pmod2 UART RXD (RXD0), P602. UM Table 19 p 27. */
  k_ra8_board_pmod2_uart_cts =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_5), /**< Pmod2 UART CTS (CTS0), P605. UM Table 19 p 27. */
  k_ra8_board_pmod2_uart_rts =
    (uint16_t)RA8_PIN(k_ra8_port_6,
                      k_ra8_pin_4), /**< Pmod2 UART RTS (RTS0), P604. UM Table 19 p 27. */
} ra8_board_pmod2_uart_pin_t;

/** @brief Pin assignments for Pmod2 GPIO + IRQ side-band (UM Table 19 p 27). */
typedef enum : uint16_t {
  k_ra8_board_pmod2_irq =
    (uint16_t)RA8_PIN(k_ra8_port_0,
                      k_ra8_pin_12), /**< Pmod2.7 IRQ (IRQ15),  P012. UM Table 19 p 27. */
  k_ra8_board_pmod2_reset =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_10), /**< Pmod2.8 RESET, P410. UM Table 19 p 27. */
  k_ra8_board_pmod2_gpio_a =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_9), /**< Pmod2.9 GPIO,  P409. UM Table 19 p 27. */
  k_ra8_board_pmod2_gpio_b =
    (uint16_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_4), /**< Pmod2.10 GPIO, P704. UM Table 19 p 27. */
} ra8_board_pmod2_gpio_pin_t;

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
 *   - Pmod1 (J26) is unused. Wi-Fi/BLE/OTA is handled by an ESP32
 *     companion IC (forthcoming), not a Pmod daughter card.
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
  k_ra8_board_mikrobus_an = (uint16_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_4),
  /** @brief J21.2 RST -- reset out to the Click. P201 also doubles as MD pin. */
  k_ra8_board_mikrobus_rst = (uint16_t)RA8_PIN(k_ra8_port_2, k_ra8_pin_1),
  /** @brief J21.3 CS   -- SPI chip-select (SSLB0, SW4-4 ON). */
  k_ra8_board_mikrobus_cs = (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_3),
  /** @brief J21.4 SCK  -- SPI clock (RSPCKB, SW4-4 ON). */
  k_ra8_board_mikrobus_sck = (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_2),
  /** @brief J21.5 CIPO -- Controller In, Peripheral Out (UM names this MISOB, SW4-4 ON). */
  /* LEGACY-OK: MISOB is the UM pin-mux signal name */
  k_ra8_board_mikrobus_cipo = (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_0),
  /** @brief J21.6 COPI -- Controller Out, Peripheral In (UM names this MOSIB, SW4-4 ON). */
  /* LEGACY-OK: MOSIB is the UM pin-mux signal name */
  k_ra8_board_mikrobus_copi = (uint16_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_1),
  /** @brief J22.1 PWM -- GTIOC10A. */
  k_ra8_board_mikrobus_pwm = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_10),
  /** @brief J22.2 INT -- hardware interrupt (IRQ-22). */
  k_ra8_board_mikrobus_int = (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_1),
  /** @brief J22.3 RX  -- UART RX (RXD7, SW4-4 ON). */
  k_ra8_board_mikrobus_rx = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_8),
  /** @brief J22.4 TX  -- UART TX (TXD7, SW4-4 ON). */
  k_ra8_board_mikrobus_tx = (uint16_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_9),
} ra8_board_mikrobus_pin_t;

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
  k_ra8_board_mikrobus_i2c_sda = (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11),
  /** @brief J22.5 SCL (SW4-5 OFF) -> P512 SCL1. */
  k_ra8_board_mikrobus_i2c_scl = (uint16_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12),
} ra8_board_mikrobus_i2c_pin_t;

/**
 * @brief I2C peripheral channel that backs the MikroBUS SDA/SCL pins.
 *
 * @details The RA8D2 group exposes a single IIC_B controller (channel 0).
 * The P511/P512 pins are its SDA1/SCL1 alternate-function pad pair (the
 * SDA0/SCL0 pads at P401/P400 are gated by SW4-5 and not used here).
 * Firmware that talks to a MikroBUS Click over I2C should call
 * ``ra8_i3c_i2c_init`` with this channel.
 */
typedef enum : uint8_t {
  k_ra8_board_mikrobus_iic_b_channel = 0U, /**< RA8 board mikrobus iic b channel. */
} ra8_board_mikrobus_iic_channel_t;

/**
 * @brief SCI channel that backs the MikroBUS J22.3/J22.4 UART pins.
 *
 * @details
 * The MikroBUS ``RX``/``TX`` pads (::k_ra8_board_mikrobus_rx = P808 /
 * ::k_ra8_board_mikrobus_tx = P809, UM Table 21 p 29) are the RXD7 / TXD7
 * alternate-function pair, i.e. SCI channel 7, enabled by SW4-4 ON. A
 * MikroE cellular Click (LTE IoT / 4G LTE / NB-IoT, all AT-command
 * modems) presents its UART on exactly these MikroBUS pins, so firmware
 * that drives an AT modem should call ``ra8_sci_init`` with this channel.
 *
 * Exposed as an enum (not a magic ``7``) so applications reference the
 * channel through the board layer rather than re-encoding the pin-mux
 * mapping. Distinct from ::k_ra8_board_uart_console_sci_channel (SCI8,
 * the J-Link OB VCOM debug console).
 */
typedef enum : uint8_t {
  k_ra8_board_mikrobus_uart_sci_channel = 7U, /**< P808/P809 -> RXD7/TXD7 = SCI7 (SW4-4 ON). */
} ra8_board_mikrobus_uart_channel_t;

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
 * | 1   | ON       | Pmod1 Mode-Sel-1 (with SW4-2 OFF -> UART; Pmod1 free). |
 * | 2   | OFF      | Pmod1 Mode-Sel-2 (UART, see Table 18).                 |
 * | 3   | ON       | Octo-SPI Inactive -- frees Pmod1/Arduino/mikroBUS.     |
 * | 4   | ON       | Arduino + mikroBUS Connectors Active (IMU on mikroBUS).|
 * | 5   | OFF      | I2C Active on mikroBUS (P511/P512 SDA1/SCL1).          |
 * | 6   | OFF      | Default -- Parallel Display + MIPI Camera.             |
 * | 7   | OFF      | Default -- USBFS role toggle in mechanical OFF.        |
 * | 8   | OFF      | Default -- USBHS in Device mode.                       |
 *
 * ``ra8_board_io_expander_apply_project_sw4_defaults`` programs this
 * pattern into the U15 PI4IOE5V6408 expander in push-pull output mode
 * (iodir=0xFF, hiz=0x00, output=0xF2), which the EK-RA8D2 UM (Sec 4.3.4
 * p 15) says overrides the SW4 DIP switches in software. The earlier
 * issue-#44 reading of ``u15: pins=0000`` was a FALSE ALARM: a 2026-06-11
 * U15 drive test (``out=FF->pins=0000 out=00->pins=0000``) shows the
 * PI4IOE5V6408 input-status register
 * (0x0F) reads 0x00 for any pin held in output mode -- it does not
 * reflect output pins -- so it never measured the mux. The override is
 * valid; the DIP does NOT need to be touched for this layout.
 *
 * @par Reference:
 * EK-RA8D2 v1 UM Rev 1.01 Table 3 p 16 (Switch Configuration Definitions)
 * and Table 18 p 26 (Pmod 1 Switch Configuration).
 */
typedef enum : uint8_t {
  /** @brief U15 output byte that drives the SW4 layout above.
   *  Bit n = 1 -> SW4-(n+1) reads OFF; bit n = 0 -> reads ON. */
  k_ra8_board_pi4ioe_output_project_default = 0xF2U,
  /** @brief U15 output byte that ELECTRICALLY ENABLES the on-board Octo-SPI
   *  flash. Written to PI4IOE5V6408 reg 0x05 (Output State). Meaning derived
   *  from the PI4IOE5V6408 datasheet register map + EK-RA8D2 v1 UM Sec 4.3.4
   *  (U15 overrides SW4): all outputs HIGH except OPMOD1/0 mode-selects
   *  (bits 0,1) and OSPI_OE_L (bit 2), which are LOW. Bit 2 = OSPI_OE_L is
   *  active-low: driving it LOW connects the flash's OM_0 bus to the MCU.
   *  Our prior 0xFF held OSPI_OE_L HIGH and kept the flash off the bus -- the
   *  root cause of issue #44. The exact U15-bit <-> SW4-channel mapping is
   *  NOT published in the UM (it lives in the EK-RA8D2 Design Package
   *  schematic); this byte is derived, and the on-hardware verification of
   *  the mapping on this EVM is tracked with the U15 bring-up work. */
  k_ra8_board_pi4ioe_output_octospi_active = 0xF8U,
  /** @brief Project layout with USBHS in HOST role: the project default
   *  (0xF2) with bit 7 (SW4-8, USBHS role) driven LOW = ON = Host, so
   *  the board supplies VBUS on the J7 jack instead of expecting it. */
  k_ra8_board_pi4ioe_output_usbhs_host = 0x72U,
} ra8_board_pi4ioe_project_t;

/* =============================================================================
 * 6d. Native SDHI bus (SDHI0, port 4 pins 0..7)
 * =============================================================================
 */

/**
 * @enum ra8_board_sdhi_pin_t
 * @brief Pin assignments for the native 4-bit SDHI0 micro-SD bus.
 *
 * @details
 * The RA8D2 routes the SDHI0 SD/MMC host-controller signals to port 4
 * pins 0..7 under ``PSEL = k_ra8_psel_sdhi`` (chip HUM Ch 20.6
 * "Multiplexed Pin Function Selector"). In bus order the eight pins are
 * CMD / CLK / DAT0 / DAT1 / DAT2 / DAT3 / WP (write-protect) /
 * CD (card-detect). The values are ``(port << 8) | pin`` encodings in
 * the ``ra8_port_pin_t`` value space, matching every other board-pin
 * enum in this header.
 *
 * @warning HARDWARE CAVEAT -- the EK-RA8D2 v1 board does NOT carry an
 * on-board micro-SD socket and the SDHI0 peripheral is not populated
 * (board UM has no SD-card table; see ``docs/MEMORY_MAP.md`` and
 * ``docs/HARDWARE_BRINGUP.md``). On EK-RA8D2 v1 these port-4 pads serve
 * CANFD / IIC functions, not SDHI. This enum and
 * ``ra8_board_sdhi_pins_init`` capture the chip-side SDHI0 pin map shared
 * by the SDHI / ra8_io demo apps; running them against real hardware
 * requires an external SDHI break-out wired to port 4. The apps that use
 * this map therefore live under ``hw_pending`` / ``_unsupported``.
 *
 * @invariant All eight members carry port index 4 (high byte 0x04).
 * @see ra8_board_sdhi_pins_init
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_ra8_board_sdhi_cmd =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_0), /**< SDHI0 CMD,  P400. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_clk =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_1), /**< SDHI0 CLK,  P401. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_dat0 =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_2), /**< SDHI0 DAT0, P402. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_dat1 =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_3), /**< SDHI0 DAT1, P403. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_dat2 =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_4), /**< SDHI0 DAT2, P404. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_dat3 =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_5), /**< SDHI0 DAT3, P405. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_wp =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_6), /**< SDHI0 WP,   P406. Chip HUM Ch 20.6. */
  k_ra8_board_sdhi_cd =
    (uint16_t)RA8_PIN(k_ra8_port_4, k_ra8_pin_7), /**< SDHI0 CD,   P407. Chip HUM Ch 20.6. */
} ra8_board_sdhi_pin_t;

/**
 * @brief SDHI instance that backs the ::ra8_board_sdhi_pin_t bus pins.
 *
 * @details
 * Exposed as a typed enum (not a macro) so applications can pass the
 * instance index to ``ra8_sdcard_init`` / ``ra8_sdhi_init`` without
 * re-encoding the literal. The port-4 pin map above is the SDHI **0**
 * function group in the chip HUM I/O Ports chapter.
 */
typedef enum : uint8_t {
  k_ra8_board_sdhi_instance = 0U, /**< SDHI0 (chip HUM Ch 20.6 SDHI pin group). */
} ra8_board_sdhi_instance_t;

/**
 * @brief Route the eight SDHI0 bus pins to the SDHI peripheral function.
 *
 * @details
 * Walks the ::ra8_board_sdhi_pin_t bus order (CMD / CLK / DAT0..3 / WP /
 * CD on port 4 pins 0..7) and calls ``ra8_pfs_route_peripheral`` for each
 * under ``PSEL = k_ra8_psel_sdhi`` (chip HUM Ch 20.6 "Multiplexed Pin
 * Function Selector"). It does NOT bring the SDHI block or the card up --
 * call ``ra8_sdcard_init`` / ``ra8_sdhi_init`` after this returns. Returns
 * on the first failing pin so the caller can panic-halt before SD
 * bring-up.
 *
 * See the @warning on ::ra8_board_sdhi_pin_t: EK-RA8D2 v1 has no on-board
 * micro-SD socket, so this routing is only meaningful with an external
 * SDHI break-out wired to port 4.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                All eight SDHI0 pins routed.
 * @retval k_ra8_err_invalid_arg   PFS programming rejected an entry.
 * @retval k_ra8_err_gpio_conflict At least one pin already owned.
 *
 * @pre IOPORT module powered (reset default).
 * @pre Single-threaded init context (no other consumer owns port-4 pins).
 * @post On success port-4 pins 0..7 are in the SDHI alternate function.
 * @post On failure the affected pins are left in their prior state.
 *
 * @note Not thread-safe; call once during board bring-up before SDHI init.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sdhi_pins_init(void);

#ifdef __cplusplus
}
#endif
