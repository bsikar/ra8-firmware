/**
 * @file ra8_board_ra8p1.h
 * @brief Board-support layer for the Renesas RA8P1 (R7KA8P1KFLCAC) target board
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Focused board-support layer for the project's RA8P1 target board, the
 * counterpart of ``libs/ra8_board_ek_ra8d2`` for the RA8D2. It names the LEDs,
 * user switches, and the debug UART console so application code can speak in
 * board coordinates ("LED1", "SW1", "the console") instead of chip coordinates
 * ("P600", "P009", "SCI8"). The public API is deliberately IDENTICAL to the
 * EK-RA8D2 board's LED / switch / console surface so a foundation app is a pure
 * board substitution (SOLID-L): only the ``#include`` changes, the calls do not.
 *
 * ## Scope of this layer
 *
 * This is a FOUNDATION board layer, not the full EK-RA8D2 connector zoo. It
 * carries only the LED / switch / console pieces the RA8P1 foundation apps
 * (``examples/ra8p1_foundation``) exercise and the first bring-up needs. The
 * EK-RA8D2-specific expansion connectors (parallel-RGB J1, Arduino, Pmod,
 * MikroBUS, OSPI/SDHI/audio/camera/Ethernet) are board wiring particular to
 * that evaluation kit and are intentionally absent here; a future RA8P1 board
 * grows its own connector map from its own schematic.
 *
 * ## Provisional pin map -- TODO(EK-RA8P1 UM / ra8p1_kicad)
 *
 * There is NO RA8P1 board in hand yet: the Renesas EK-RA8P1 User's Manual is
 * not committed to ``docs/reference/`` and the project's own RA8P1 PCB
 * (``ra8p1_kicad/``) does not exist. The RA8P1 is pin-compatible with the RA8D2
 * in the same 289-pin BGA and register/pin-function-identical (see
 * ``libs/ra8_core/inc/ra8_device.h`` and the RA8P1 HUM R01UH1064EJ), so the
 * LED / switch / console pins below are PROVISIONALLY mirrored from the
 * EK-RA8D2 layout: every pin is a valid GPIO / SCI alternate on the RA8P1
 * (chip HUM R01UH1064EJ Ch 20 "I/O Ports"). Each assignment carries a
 * ``TODO(EK-RA8P1 UM / ra8p1_kicad)`` marker: the board-level "which LED sits on
 * which pin" is a schematic decision that is re-derived, and re-cited to the
 * board manual, once the RA8P1 board is defined and the first on-silicon
 * bring-up runs. This layer builds and runs in ``ra8_emulator --device ra8p1``
 * today; on-silicon validation is deferred to that board.
 *
 * Underlying chip register access is delegated to ``libs/ra8_hal``: the BSP
 * itself does NO register pokes -- it translates board names into the right
 * ``ra8_port_pin_t`` / ``ra8_psel_t`` / ``ra8_icu_irq_cfg_t`` values and forwards
 * to the HAL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * Returned by ``ra8_board_get_info()`` so applications can log / verify the
 * board they were built for. All three are ASCII string literals with
 * permanent storage duration.
 */
extern const char* const k_ra8_board_name;    /**< "RA8P1 foundation board". */
extern const char* const k_ra8_board_doc_rev; /**< "R01UH1064EJ (chip HUM)". */
extern const char* const k_ra8_board_mcu;     /**< "R7KA8P1KFLCAC".          */

/**
 * @struct ra8_board_info_t
 * @brief Snapshot of board identity strings.
 *
 * @details
 * A caller passes a stack instance to ``ra8_board_get_info`` which fills the
 * three pointers with the ``k_ra8_board_*`` globals. Layout mirrors the
 * EK-RA8D2 ``ra8_board_info_t`` so identity-printing helpers are board-agnostic.
 *
 * @invariant On a successful fill, every pointer is non-NULL and points at a
 *            permanent ASCII literal.
 * @code
 * ra8_board_info_t info = {};
 * (void)ra8_board_get_info(&info);
 * // info.mcu == "R7KA8P1KFLCAC"
 * @endcode
 * @see ra8_board_get_info
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
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Filled.
 * @retval k_ra8_err_invalid_arg    out == NULL.
 *
 * @pre out is writable.
 * @pre out has storage for three pointers.
 * @post On success out->name / doc_rev / mcu point at the global
 *       ``k_ra8_board_*`` strings.
 * @post On failure out is left unmodified.
 *
 * @note Not thread-safe with respect to nothing -- pure copy of const globals.
 * @see k_ra8_board_name
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_get_info(ra8_board_info_t* out);

/* =============================================================================
 * 1. User LEDs -- provisional, TODO(EK-RA8P1 UM / ra8p1_kicad)
 * =============================================================================
 */

/**
 * @enum ra8_board_led_id_t
 * @brief Identifiers for the three user LEDs on the RA8P1 foundation board.
 *
 * @details
 * PROVISIONAL pins mirrored from the EK-RA8D2 layout (P600 / P303 / PA07); all
 * three are valid GPIO outputs on the pin-compatible RA8P1 (chip HUM
 * R01UH1064EJ Ch 20 "I/O Ports"). All three are treated as active-high.
 * TODO(EK-RA8P1 UM / ra8p1_kicad): confirm the LED-to-pin mapping and polarity
 * against the RA8P1 board schematic once it exists.
 *
 * @invariant Enumerators are contiguous 0..k_ra8_board_led_count-1.
 * @see ra8_board_led_init
 */
typedef enum : uint8_t {
  k_ra8_board_led1 = 0U, /**< LED1: provisional P600. TODO(EK-RA8P1 UM / ra8p1_kicad). */
  k_ra8_board_led2 = 1U, /**< LED2: provisional P303. TODO(EK-RA8P1 UM / ra8p1_kicad). */
  k_ra8_board_led3 = 2U, /**< LED3: provisional PA07. TODO(EK-RA8P1 UM / ra8p1_kicad). */

  /** Convenience aliases by the EK-RA8D2 colour order (also provisional). */
  k_ra8_board_led_blue  = k_ra8_board_led1,
  k_ra8_board_led_green = k_ra8_board_led2, /**< RA8 board led green. */
  k_ra8_board_led_red   = k_ra8_board_led3, /**< RA8 board led red.   */

  k_ra8_board_led_count = 3U, /**< Number of user LEDs. */
} ra8_board_led_id_t;

/**
 * @brief Configure ``led`` as a digital output, initial level low (off).
 *
 * @param[in] led LED identifier (< k_ra8_board_led_count).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Pin claimed and driven low.
 * @retval k_ra8_err_invalid_arg     led >= k_ra8_board_led_count.
 * @retval k_ra8_err_gpio_conflict   Pin already owned.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @pre led is a valid ::ra8_board_led_id_t enumerator.
 * @post On success the LED is off and its pin is a digital output.
 * @post On failure no pin state changes.
 *
 * @note Not thread-safe; call from the single boot / render context.
 * @see ra8_board_led_on
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_init(ra8_board_led_id_t led);

/**
 * @brief Drive ``led`` HIGH (light it).
 * @param[in] led LED identifier (< k_ra8_board_led_count).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Output latch set high.
 * @retval k_ra8_err_invalid_arg  led out of range.
 * @pre ra8_board_led_init(led) succeeded.
 * @pre led is a valid enumerator.
 * @post LED pin output latch == 1.
 * @post No other pin changes.
 * @note Not thread-safe.
 * @see ra8_board_led_off
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_on(ra8_board_led_id_t led);

/**
 * @brief Drive ``led`` LOW (extinguish it).
 * @param[in] led LED identifier (< k_ra8_board_led_count).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Output latch cleared.
 * @retval k_ra8_err_invalid_arg  led out of range.
 * @pre ra8_board_led_init(led) succeeded.
 * @pre led is a valid enumerator.
 * @post LED pin output latch == 0.
 * @post No other pin changes.
 * @note Not thread-safe.
 * @see ra8_board_led_on
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_off(ra8_board_led_id_t led);

/**
 * @brief Toggle ``led``'s output state.
 * @param[in] led LED identifier (< k_ra8_board_led_count).
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Output latch inverted.
 * @retval k_ra8_err_invalid_arg  led out of range.
 * @pre ra8_board_led_init(led) succeeded.
 * @pre led is a valid enumerator.
 * @post LED pin output latch is inverted from its prior value.
 * @post No other pin changes.
 * @note Not thread-safe.
 * @see ra8_board_led_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_toggle(ra8_board_led_id_t led);

/**
 * @brief Translate a board LED id into its underlying ``ra8_port_pin_t``.
 *
 * @param[in]  led      LED identifier (< k_ra8_board_led_count).
 * @param[out] out_pin  Destination pin id; must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               *out_pin holds the mapped pin.
 * @retval k_ra8_err_invalid_arg  led out of range or out_pin NULL.
 *
 * @pre out_pin is writable.
 * @pre led is a valid enumerator.
 * @post On success *out_pin is the provisional pin for led.
 * @post On failure *out_pin is unmodified.
 *
 * @note Pure lookup; ISR-safe.
 * @see ra8_board_led_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_led_pin(ra8_board_led_id_t led, ra8_port_pin_t* out_pin);

/* =============================================================================
 * 2. User switches -- provisional, TODO(EK-RA8P1 UM / ra8p1_kicad)
 * =============================================================================
 */

/**
 * @enum ra8_board_sw_id_t
 * @brief Identifiers for the on-board user push-buttons.
 *
 * @details
 * PROVISIONAL pins mirrored from the EK-RA8D2 layout (SW1 -> P009, SW2 -> P008);
 * both are valid IRQ-capable GPIO inputs on the pin-compatible RA8P1 (chip HUM
 * R01UH1064EJ Ch 20 "I/O Ports"). Both are wired active-low.
 * TODO(EK-RA8P1 UM / ra8p1_kicad): confirm the switch-to-pin mapping against the
 * RA8P1 board schematic once it exists.
 *
 * @invariant Enumerators are contiguous 0..k_ra8_board_sw_count-1.
 * @see ra8_board_sw_init
 */
typedef enum : uint8_t {
  k_ra8_board_sw1      = 0U, /**< SW1: provisional P009 (IRQ13). TODO(EK-RA8P1 UM / ra8p1_kicad). */
  k_ra8_board_sw2      = 1U, /**< SW2: provisional P008 (IRQ12). TODO(EK-RA8P1 UM / ra8p1_kicad). */
  k_ra8_board_sw_count = 2U, /**< Number of user switches.                                        */
} ra8_board_sw_id_t;

/**
 * @enum ra8_board_sw_irq_t
 * @brief Per-switch ICU IRQ channel numbers (provisional).
 *
 * @details
 * SW1 -> IRQ13, SW2 -> IRQ12, mirrored from the EK-RA8D2 layout. Values are
 * bare IRQ channel numbers usable with ``ra8_icu_configure_irq_pin``.
 * TODO(EK-RA8P1 UM / ra8p1_kicad): re-confirm the IRQ channel per pin.
 *
 * @invariant Each value is a valid ICU external-IRQ channel (0..31).
 * @see ra8_board_sw_attach_irq
 */
typedef enum : uint8_t {
  k_ra8_board_sw1_irq = 13U, /**< SW1 -> IRQ13. TODO(EK-RA8P1 UM / ra8p1_kicad). */
  k_ra8_board_sw2_irq = 12U, /**< SW2 -> IRQ12. TODO(EK-RA8P1 UM / ra8p1_kicad). */
} ra8_board_sw_irq_t;

/**
 * @enum ra8_board_sw_state_t
 * @brief Logical "pressed" / "released" state of a user switch.
 *
 * @invariant Exactly one of the two values.
 * @see ra8_board_sw_read
 */
typedef enum : uint8_t {
  k_ra8_board_sw_released = 0U, /**< Button not held (pin high, active-low). */
  k_ra8_board_sw_pressed  = 1U, /**< Button held (pin low, active-low).      */
} ra8_board_sw_state_t;

/**
 * @brief Configure a switch pin as an input with internal pull-up.
 *
 * @param[in] sw Switch id (< k_ra8_board_sw_count).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Pin configured as pulled-up input.
 * @retval k_ra8_err_invalid_arg    sw out of range.
 * @retval k_ra8_err_gpio_conflict  Pin already owned.
 *
 * @pre HAL pin validator initialized.
 * @pre sw is a valid enumerator.
 * @post On success the pin is a digital input with pull-up enabled.
 * @post On failure no pin state changes.
 *
 * @note Not thread-safe.
 * @see ra8_board_sw_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_init(ra8_board_sw_id_t sw);

/**
 * @brief Sample the current state of ``sw``.
 *
 * @details
 * The buttons are wired active-low (a press shorts the pin to GND); this
 * function inverts the level so callers receive a positive
 * "pressed = true" semantic.
 *
 * @param[in]  sw           Switch id (< k_ra8_board_sw_count).
 * @param[out] out_pressed  Set to one of {released, pressed}; must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Read complete.
 * @retval k_ra8_err_invalid_arg    sw out of range or out_pressed NULL.
 *
 * @pre ra8_board_sw_init(sw) succeeded.
 * @pre out_pressed is writable.
 * @post On success *out_pressed is one of {released, pressed}.
 * @post On failure *out_pressed is unmodified.
 *
 * @note Not thread-safe.
 * @see ra8_board_sw_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_read(ra8_board_sw_id_t sw, ra8_board_sw_state_t* out_pressed);

/**
 * @brief Switch IRQ callback signature.
 * @param[in,out] ctx Opaque context pointer passed at registration.
 */
typedef void (*ra8_board_sw_irq_cb_t)(void* ctx);

/**
 * @brief Wire ``sw`` to the ICU and register a falling-edge callback.
 *
 * @details
 * Buttons are active-low so a falling edge corresponds to a press. The HAL
 * handles the PFS routing, IRQCR programming, and NVIC enable; this veneer
 * hides the specific channel number from the caller.
 *
 * @param[in]     sw  Switch id (< k_ra8_board_sw_count).
 * @param[in]     cb  Falling-edge callback; must be non-NULL.
 * @param[in,out] ctx Opaque context forwarded to ``cb``; may be NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 IRQ wired and callback registered.
 * @retval k_ra8_err_invalid_arg    sw out of range or cb NULL.
 * @retval k_ra8_err_gpio_conflict  Underlying pin/IRQ already owned.
 *
 * @pre ra8_icu_init() has been called once during boot.
 * @pre cb is non-NULL.
 * @post On success pressing ``sw`` invokes ``cb(ctx)`` from the ICU ISR.
 * @post On failure no IRQ or NVIC line is enabled for sw.
 *
 * @note Not thread-safe.
 * @see ra8_board_sw_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_board_sw_attach_irq(ra8_board_sw_id_t sw, ra8_board_sw_irq_cb_t cb, void* ctx);

/**
 * @brief Translate a board switch id into its underlying ``ra8_port_pin_t``.
 *
 * @param[in]  sw       Switch id (< k_ra8_board_sw_count).
 * @param[out] out_pin  Destination pin id; must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               *out_pin holds the mapped pin.
 * @retval k_ra8_err_invalid_arg  sw out of range or out_pin NULL.
 *
 * @pre out_pin is writable.
 * @pre sw is a valid enumerator.
 * @post On success *out_pin is the provisional pin for sw.
 * @post On failure *out_pin is unmodified.
 *
 * @note Pure lookup; ISR-safe.
 * @see ra8_board_sw_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_sw_pin(ra8_board_sw_id_t sw, ra8_port_pin_t* out_pin);

/* =============================================================================
 * 3. Debug UART console -- provisional, TODO(EK-RA8P1 UM / ra8p1_kicad)
 * =============================================================================
 */

/**
 * @enum ra8_board_uart_t
 * @brief Identifiers for board-routed UART consoles.
 *
 * @details
 * The RA8P1 foundation board exposes one debug console, provisionally routed
 * exactly like the EK-RA8D2 J-Link OB VCOM bridge: PD02 (TXD) / PD03 (RXD) on
 * SCI8. On the RA8P1 PD02/PD03 are the SCI8 TXD8/RXD8 alternate functions (chip
 * HUM R01UH1064EJ Ch 20 "I/O Ports", Multiplexed Pin Function Selector).
 * TODO(EK-RA8P1 UM / ra8p1_kicad): confirm the VCOM pins / SCI channel against
 * the RA8P1 board schematic once it exists.
 *
 * @invariant Single enumerator (one console).
 * @see ra8_board_uart_console_init
 */
typedef enum : uint8_t {
  k_ra8_board_uart_console = 0U, /**< Debug VCOM console: provisional PD02/PD03 on SCI8. */
} ra8_board_uart_t;

/**
 * @enum ra8_board_uart_sci_channel_t
 * @brief SCI channel that backs ``k_ra8_board_uart_console`` (provisional).
 *
 * @details
 * Exposed as a typed enum (not a macro) so applications reference the channel
 * without re-encoding the number. Provisional value 8 (SCI8), mirrored from the
 * EK-RA8D2 console. TODO(EK-RA8P1 UM / ra8p1_kicad): confirm.
 *
 * @invariant Value is a valid SCI channel index.
 * @see ra8_board_uart_console_init
 */
typedef enum : uint8_t {
  k_ra8_board_uart_console_sci_channel = 8U, /**< PD02/PD03 -> SCI8 (provisional). */
} ra8_board_uart_sci_channel_t;

/**
 * @enum ra8_board_uart_console_pin_t
 * @brief Pin assignments for the debug VCOM console (provisional).
 *
 * @details
 * PD02 (TXD) / PD03 (RXD) are the always-wired console lines; PD04 (RTS) /
 * PD05 (CTS) are the optional hardware-flow-control lines. Chip-coordinate
 * names: P1302 / P1303 / P1304 / P1305 (port 13, pins 2..5). Mirrored from the
 * EK-RA8D2 layout. TODO(EK-RA8P1 UM / ra8p1_kicad): confirm.
 *
 * @invariant Each value is a ``(port << 8) | pin`` encoding on port 13.
 * @see ra8_board_uart_console_init
 */
typedef enum : uint16_t {
  k_ra8_board_uart_console_pin_txd =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_2), /**< PD02 TXD (provisional). */
  k_ra8_board_uart_console_pin_rxd =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_3), /**< PD03 RXD (provisional). */
  k_ra8_board_uart_console_pin_rts =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_4), /**< PD04 RTS (provisional). */
  k_ra8_board_uart_console_pin_cts =
    (uint16_t)RA8_PIN(k_ra8_port_13, k_ra8_pin_5), /**< PD05 CTS (provisional). */
} ra8_board_uart_console_pin_t;

/**
 * @brief Configure SCI8 + PD02/PD03 as the debug-console UART.
 *
 * @details
 * Routes PD02 -> TXD8 and PD03 -> RXD8 (PSEL = SCI async) and brings SCI8 up via
 * ``ra8_sci_init`` with 8N1 framing at the requested baud. The SCI module
 * operating clock on the RA8P1 is PCLKA (byte-identical to the RA8D2 SCI_B); the
 * BRR divisor is computed from the CURRENT PCLKA frequency reported by
 * ``ra8_cgc_get_clock_hz``, so callers MUST call ``ra8_cgc_init()`` before this.
 *
 * @param[in] baud Target line rate in bps (e.g. 115200); must be non-zero.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Console up, ready to TX/RX.
 * @retval k_ra8_err_invalid_arg     baud == 0.
 * @retval k_ra8_err_not_initialized CGC has not published a usable PCLKA yet.
 * @retval k_ra8_err_gpio_conflict   PD02 or PD03 already owned.
 * @retval k_ra8_err_hw_init_failed  Underlying ``ra8_sci_init`` failed.
 *
 * @pre HAL pin validator initialized (single-threaded boot context).
 * @pre ``ra8_cgc_init()`` has run and PCLKA is post-PLL.
 * @post On success SCI8 is enabled (TE=RE=1) and PD02/PD03 route to SCI8.
 * @post On failure the console stays uninitialised and writes are refused.
 *
 * @note Not thread-safe; call once during board bring-up.
 * @see ra8_board_uart_console_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_init(uint32_t baud);

/**
 * @brief Polled blocking write to the debug VCOM console.
 *
 * @param[in] data Bytes to transmit; must be non-NULL when ``len`` > 0.
 * @param[in] len  Number of bytes in ``data``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  All bytes pushed to the SCI8 TDR (or len 0).
 * @retval k_ra8_err_invalid_arg     data NULL with non-zero len.
 * @retval k_ra8_err_not_initialized console_init not called.
 *
 * @pre ra8_board_uart_console_init succeeded (for len > 0).
 * @pre data covers len bytes.
 * @post On success all len bytes have been handed to the SCI8 TDR.
 * @post On failure no bytes are transmitted.
 *
 * @note Not thread-safe.
 * @see ra8_board_uart_console_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_write(const uint8_t* data, size_t len);

/**
 * @brief Polled non-blocking read from the debug VCOM console.
 *
 * @details
 * Drains up to ``cap`` bytes from SCI8 RDR while data is available. Stops
 * (without error) the first time the SCI reports no byte so the call never
 * blocks waiting for a silent host.
 *
 * @param[out] out      Destination buffer (non-NULL when ``cap`` > 0).
 * @param[in]  cap      Capacity of ``out`` in bytes.
 * @param[out] out_len  Number of bytes actually read; must be non-NULL.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Read complete; *out_len in [0, cap].
 * @retval k_ra8_err_invalid_arg     out / out_len NULL with non-zero cap.
 * @retval k_ra8_err_not_initialized console_init not called.
 *
 * @pre ra8_board_uart_console_init succeeded (for cap > 0).
 * @pre out covers cap bytes.
 * @post On success 0 <= *out_len <= cap.
 * @post On failure *out_len is 0 (when writable).
 *
 * @note Not thread-safe.
 * @see ra8_board_uart_console_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len);

/**
 * @brief Block until every byte queued on the console has clocked out.
 *
 * @details
 * Thin wrapper around ``ra8_sci_flush(k_ra8_board_uart_console_sci_channel)``
 * that polls the SCI8 transmit-complete status. Intended for a panic handler
 * that must get its failure log to the host before WFI gates the SCI clock.
 * The CSR.TEND register poke lives in ``ra8_sci_flush`` (HAL), which carries the
 * HUM citation; this veneer touches no registers.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                  Transmit complete (or fake stub).
 * @retval k_ra8_err_not_initialized console_init not called.
 * @retval k_ra8_err_hw_timeout      Spin budget elapsed without completion.
 *
 * @pre ra8_board_uart_console_init succeeded.
 * @pre The caller has previously written bytes to drain.
 * @post On success every prior console byte has been transmitted.
 * @post On failure the console state is unchanged.
 *
 * @note Not thread-safe with respect to a concurrent write.
 * @see ra8_board_uart_console_write
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_uart_console_flush(void);

#ifdef __cplusplus
}
#endif
