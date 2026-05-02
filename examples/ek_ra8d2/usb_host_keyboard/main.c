/**
 * @file main.c
 * @brief USB host-mode HID keyboard smoke test for EK-RA8D2 (USB-HS)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * Hardware-test app for the host-side HID class layer (`ra_usb_hhid.c`).
 * Turns the EK-RA8D2 into a USB host on its USB-HS receptacle (J7
 * Type-C on EK-RA8D2 v1) so the operator can plug a USB keyboard into
 * it, type, and watch the decoded characters echo on the J-Link OB CDC
 * virtual COM port at 115200 8N1.
 *
 * Sequence:
 *
 *   1. `ra_cgc_init()` -- XTAL -> PLL1, CPUCLK0 = 1 GHz.
 *   2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
 *   3. `ra_pfs_route_peripheral()` for SCI8 (PD_02 / PD_03) and
 *      `ra_sci_init(8, ...)` for the J-Link OB CDC log channel.
 *   4. `ra_pfs_route_peripheral()` for P4_08 with PSEL = 0x14
 *      (USBHS_VBUS sense).
 *   5. `ra_gpio_output_init` for LED1 + LED2.
 *   6. `ra_usb_hhid_init(k_ra_usb_speed_hs)` -- flips USBHS to host
 *      mode and arms the chapter-9 step machine.
 *   7. `ra_usb_hhid_attach_callback()` records the device snapshot and
 *      lights LED1 on attach.
 *   8. After attach, main calls
 *      `ra_usb_hhid_set_protocol(k_ra_hhid_proto_boot)` and
 *      `ra_usb_hhid_set_idle(0, 0)` so the keyboard speaks the legacy
 *      8-byte boot-protocol input report (USB HID 1.11 sec B.1).
 *   9. Main loops at 10 ms intervals issuing
 *      `ra_usb_hhid_get_input_report()`. Each fresh report is decoded:
 *      byte 0 = modifier bitmap (Shift / Ctrl / Alt / GUI),
 *      bytes 2..7 = up to six pressed keycodes. New keycodes that were
 *      not in the previous report are decoded to ASCII via the
 *      lookup table below and pushed out SCI8. LED2 toggles per
 *      keystroke.
 *
 * HID-keycode -> ASCII lookup (USB HID Usage Tables 1.4 sec 10
 * "Keyboard / Keypad Page (0x07)"):
 *
 *  - 0x04..0x1D : letters 'a'..'z' (Shift -> uppercase 'A'..'Z')
 *  - 0x1E..0x26 : digits '1'..'9' (Shift -> '!@#$%^&*(' )
 *  - 0x27       : digit '0'        (Shift -> ')')
 *  - 0x28       : '\n' (Enter)
 *  - 0x29       : ESC (printed as '^[')
 *  - 0x2A       : Backspace ('\b')
 *  - 0x2B       : Tab ('\t')
 *  - 0x2C       : space (' ')
 *  - 0x2D       : '-' (Shift -> '_')
 *  - 0x2E       : '=' (Shift -> '+')
 *  - 0x2F       : '[' (Shift -> '{')
 *  - 0x30       : ']' (Shift -> '}')
 *  - 0x31       : '\\' (Shift -> '|')
 *  - 0x33       : ';' (Shift -> ':')
 *  - 0x34       : '\'' (Shift -> '"')
 *  - 0x35       : '`' (Shift -> '~')
 *  - 0x36       : ',' (Shift -> '<')
 *  - 0x37       : '.' (Shift -> '>')
 *  - 0x38       : '/' (Shift -> '?')
 *
 * The modifier byte 0 carries the Shift bits at 0x02 (Left-Shift) and
 * 0x20 (Right-Shift). Other modifiers are not decoded; the table above
 * is intentionally limited to printable ASCII so the test reads
 * cleanly on a serial terminal.
 *
 * Verification: open the J-Link OB CDC port at 115200 8N1
 * (`picocom -b 115200 /dev/cu.usbmodem*`), reset the EVM, plug a USB
 * keyboard into J7. The line "ra8d2 host: device attached vid=...
 * pid=..." prints, LED1 lights, and typing on the keyboard echoes the
 * pressed keys to picocom. LED2 blinks per keystroke.
 *
 * @par Architectural ring
 * [Ring 6 / APP] {World: S} -- application-layer code that runs in
 * the Secure world.
 *
 * @author Brighton Sikarskie
 * @date 2026-04-29
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"
#include "ra_usb.h"
#include "ra_usb_hhid.h"

/* =============================================================================
 * Compile-time configuration
 * =============================================================================
 */

/**
 * @enum usb_host_kbd_config_t
 * @brief Compile-time settings for the host-HID keyboard demo.
 */
typedef enum : uint32_t {
  k_usb_kbd_baud        = 115200U, /**< J-Link OB CDC log baud.        */
  k_usb_kbd_sci_channel = 8U,      /**< SCI8 -> J-Link OB CDC bridge.  */
  k_usb_kbd_poll_ms     = 10U,     /**< Interrupt-IN poll cadence.     */
} usb_host_kbd_config_t;

/**
 * @enum usb_host_kbd_report_t
 * @brief Boot-protocol HID keyboard report layout.
 *
 * @details Per USB HID 1.11 sec B.1 "Boot Keyboard Report Format":
 * 8 bytes total: modifier bitmap (1) + reserved (1) + 6 keycode slots.
 */
typedef enum : uint8_t {
  k_usb_kbd_report_bytes          = 8U, /**< Total report length.        */
  k_usb_kbd_report_idx_mod        = 0U, /**< Modifier bitmap byte index.*/
  k_usb_kbd_report_idx_keys_first = 2U, /**< First keycode slot.  */
  k_usb_kbd_report_idx_keys_last  = 7U, /**< Last keycode slot.   */
  k_usb_kbd_report_keys_count     = 6U, /**< Slot count.          */
} usb_host_kbd_report_t;

/**
 * @enum usb_host_kbd_modifier_t
 * @brief Bit flags inside the boot-protocol modifier byte (HID 1.11
 *        sec B.1).
 */
typedef enum : uint8_t {
  k_usb_kbd_mod_lctrl  = 0x01U, /**< Left-Control.   */
  k_usb_kbd_mod_lshift = 0x02U, /**< Left-Shift.     */
  k_usb_kbd_mod_lalt   = 0x04U, /**< Left-Alt.       */
  k_usb_kbd_mod_lgui   = 0x08U, /**< Left-GUI / Win. */
  k_usb_kbd_mod_rctrl  = 0x10U, /**< Right-Control.  */
  k_usb_kbd_mod_rshift = 0x20U, /**< Right-Shift.    */
  k_usb_kbd_mod_ralt   = 0x40U, /**< Right-Alt.      */
  k_usb_kbd_mod_rgui   = 0x80U, /**< Right-GUI.      */
} usb_host_kbd_modifier_t;

/**
 * @enum usb_host_kbd_keycode_t
 * @brief Keyboard usage IDs that the decoder recognises.
 *
 * @details From USB HID Usage Tables 1.4 sec 10 "Keyboard/Keypad
 * Page". The decoder ignores anything outside this range.
 */
typedef enum : uint8_t {
  k_usb_kbd_kc_a     = 0x04U, /**< 'a'. */
  k_usb_kbd_kc_z     = 0x1DU, /**< 'z'. */
  k_usb_kbd_kc_1     = 0x1EU, /**< '1'. */
  k_usb_kbd_kc_9     = 0x26U, /**< '9'. */
  k_usb_kbd_kc_0     = 0x27U, /**< '0'. */
  k_usb_kbd_kc_enter = 0x28U, /**< Enter.        */
  k_usb_kbd_kc_esc   = 0x29U, /**< Escape.       */
  k_usb_kbd_kc_bksp  = 0x2AU, /**< Backspace.    */
  k_usb_kbd_kc_tab   = 0x2BU, /**< Tab.          */
  k_usb_kbd_kc_space = 0x2CU, /**< Space.        */
  k_usb_kbd_kc_minus = 0x2DU, /**< '-'. */
  k_usb_kbd_kc_equal = 0x2EU, /**< '='. */
  k_usb_kbd_kc_lbrk  = 0x2FU, /**< '['. */
  k_usb_kbd_kc_rbrk  = 0x30U, /**< ']'. */
  k_usb_kbd_kc_bsl   = 0x31U, /**< '\\'.*/
  k_usb_kbd_kc_semi  = 0x33U, /**< ';'. */
  k_usb_kbd_kc_quote = 0x34U, /**< '\''.*/
  k_usb_kbd_kc_grave = 0x35U, /**< '`'. */
  k_usb_kbd_kc_comma = 0x36U, /**< ','. */
  k_usb_kbd_kc_dot   = 0x37U, /**< '.'. */
  k_usb_kbd_kc_slash = 0x38U, /**< '/'. */
} usb_host_kbd_keycode_t;

/**
 * @enum usb_host_kbd_psel_t
 * @brief PFS PSEL code for routing P4_08 to USBHS_VBUS.
 *
 * @details HUM Ch 20.2 "PFS register PSEL field encoding" gives 0x14
 * for USBHS.
 */
typedef enum : uint8_t {
  k_usb_kbd_psel_usb_hs =
    (uint8_t)k_ra_psel_usb_hs, /* HUM Ch 20.6 "Peripheral Select Settings" p 855 */
} usb_host_kbd_psel_t;

/**
 * @enum usb_host_kbd_hex_t
 * @brief Hex text-buffer sizing.
 */
typedef enum : uint8_t {
  k_usb_kbd_hex_chars_u16    = 4U, /**< 16-bit value -> "ABCD".  */
  k_usb_kbd_hex_nibble_count = 4U, /**< Bits per hex nibble.     */
} usb_host_kbd_hex_t;

/**
 * @enum usb_host_kbd_hex_mask_t
 * @brief Bit-mask constants used by the hex formatter.
 */
typedef enum : uint32_t {
  k_usb_kbd_hex_nibble_mask = 0xFU, /**< 4-bit nibble mask.            */
  k_usb_kbd_hex_digit_split = 10U,  /**< Threshold between '0-9'/'A-F'.*/
} usb_host_kbd_hex_mask_t;

/* =============================================================================
 * Pin assignments
 * =============================================================================
 */

/** @brief J-Link OB CDC TX pin (PD_02 -- SCI8 TX). */
static const ra_port_pin_t k_usb_kbd_pin_sci_tx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);

/** @brief J-Link OB CDC RX pin (PD_03 -- SCI8 RX). */
static const ra_port_pin_t k_usb_kbd_pin_sci_rx =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief USBHS_VBUS sense pin (P4_08, PSEL = 0x14). */
static const ra_port_pin_t k_usb_kbd_pin_hs_vbus =
  (ra_port_pin_t)(((uint16_t)k_ra_port_4 << 8) | (uint16_t)k_ra_pin_8);

/* =============================================================================
 * Status messages over SCI8
 * =============================================================================
 */

/** @brief First message after SCI is up. */
static const uint8_t k_usb_kbd_msg_ready[] = "ra8d2 host: ready, plug a USB keyboard into J7\r\n";

/** @brief Static prefix for the per-attach VID/PID print. */
static const uint8_t k_usb_kbd_msg_attach_pre[] = "ra8d2 host: device attached vid=0x";

/** @brief Mid-message separator for the per-attach print (PID). */
static const uint8_t k_usb_kbd_msg_attach_mid[] = " pid=0x";

/** @brief Trailing CR/LF after a print. */
static const uint8_t k_usb_kbd_msg_crlf[] = "\r\n";

/** @brief Printed instead of an ESC keypress (so terminals don't choke). */
static const uint8_t k_usb_kbd_msg_esc[] = "^[";

/* =============================================================================
 * HID -> ASCII lookup tables
 * =============================================================================
 */

/**
 * @enum usb_host_kbd_table_t
 * @brief Sizing constants for the keycode -> ASCII tables.
 */
typedef enum : uint8_t {
  k_usb_kbd_table_lo  = 0x04U, /**< First decoded usage. */
  k_usb_kbd_table_hi  = 0x38U, /**< Last  decoded usage. */
  k_usb_kbd_table_len = (uint8_t)(0x38U - 0x04U + 1U),
  k_usb_kbd_glyph_esc = 0x1BU, /**< ASCII ESC, signals multi-byte CSI emit. */
} usb_host_kbd_table_t;

/**
 * @brief Unshifted keycode -> ASCII table covering 0x04..0x38.
 *
 * @details Index = `keycode - k_usb_kbd_table_lo`. A 0 entry means the
 * keycode is not decoded (we still toggle LED2 + count it as a
 * keystroke for the LED, just nothing is printed).
 */
static const uint8_t k_usb_kbd_ascii_unshifted[k_usb_kbd_table_len] = {
  /* 0x04..0x1D : 'a'..'z' */
  'a',
  'b',
  'c',
  'd',
  'e',
  'f',
  'g',
  'h',
  'i',
  'j',
  'k',
  'l',
  'm',
  'n',
  'o',
  'p',
  'q',
  'r',
  's',
  't',
  'u',
  'v',
  'w',
  'x',
  'y',
  'z',
  /* 0x1E..0x26 : '1'..'9' */
  '1',
  '2',
  '3',
  '4',
  '5',
  '6',
  '7',
  '8',
  '9',
  /* 0x27       : '0' */
  '0',
  /* 0x28..0x2C : Enter, Esc, BS, Tab, Space */
  '\n',
  0x1BU /* Esc placeholder */,
  '\b',
  '\t',
  ' ',
  /* 0x2D..0x32 : - = [ ] \ # */
  '-',
  '=',
  '[',
  ']',
  '\\',
  0U /* non-US #/~ */,
  /* 0x33..0x38 : ; ' ` , . / */
  ';',
  '\'',
  '`',
  ',',
  '.',
  '/',
};

/**
 * @brief Shifted keycode -> ASCII table covering 0x04..0x38.
 *
 * @details Index = `keycode - k_usb_kbd_table_lo`. A 0 entry means
 * "no shifted glyph"; we fall back to the unshifted entry so e.g.
 * Shift-Enter still produces '\n'.
 */
static const uint8_t k_usb_kbd_ascii_shifted[k_usb_kbd_table_len] = {
  /* 0x04..0x1D : 'A'..'Z' */
  'A',
  'B',
  'C',
  'D',
  'E',
  'F',
  'G',
  'H',
  'I',
  'J',
  'K',
  'L',
  'M',
  'N',
  'O',
  'P',
  'Q',
  'R',
  'S',
  'T',
  'U',
  'V',
  'W',
  'X',
  'Y',
  'Z',
  /* 0x1E..0x26 : '!@#$%^&*(' */
  '!',
  '@',
  '#',
  '$',
  '%',
  '^',
  '&',
  '*',
  '(',
  /* 0x27       : ')' */
  ')',
  /* 0x28..0x2C : Enter, Esc, BS, Tab, Space (unchanged) */
  '\n',
  0x1BU,
  '\b',
  '\t',
  ' ',
  /* 0x2D..0x32 : _ + { } | ~ */
  '_',
  '+',
  '{',
  '}',
  '|',
  0U,
  /* 0x33..0x38 : : " ~ < > ? */
  ':',
  '"',
  '~',
  '<',
  '>',
  '?',
};

/* =============================================================================
 * Attach state shared with the callback
 * =============================================================================
 */

/**
 * @struct usb_host_kbd_state_t
 * @brief Mutable state set by the attach callback and read by main.
 */
typedef struct {
  bool                 attached; /**< True after attach callback fires. */
  ra_usb_hhid_device_t device;   /**< Snapshot of the attached device.  */
} usb_host_kbd_state_t;

/** @brief File-scope state shared between callback + main loop. */
static volatile usb_host_kbd_state_t s_state = {};

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Halt forever in WFI -- panic stop on init failure.
 *
 * @pre Called only after a fatal error in boot.
 *
 * @post CPU is parked; only a debugger or external reset wakes it.
 *
 * @since 0.1.0
 */
static void usb_kbd_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Format a uint16_t into 4 uppercase hex characters.
 *
 * @param[in]  value 16-bit value.
 * @param[out] out   Destination buffer (>=4 bytes).
 *
 * @pre `out` has 4 bytes of storage.
 *
 * @post `out[0..3]` hold the big-endian hex digits of `value`.
 *
 * @since 0.1.0
 */
static void usb_kbd_format_hex_u16(uint16_t value, uint8_t* out)
{
  for (uint8_t i = 0U; i < k_usb_kbd_hex_chars_u16; i++) {
    const uint8_t shift =
      (uint8_t)((k_usb_kbd_hex_chars_u16 - 1U - i) * k_usb_kbd_hex_nibble_count);
    const uint32_t nibble = ((uint32_t)value >> shift) & k_usb_kbd_hex_nibble_mask;
    out[i] = (uint8_t)((nibble < k_usb_kbd_hex_digit_split)
                         ? ((uint8_t)'0' + (uint8_t)nibble)
                         : ((uint8_t)'A' + (uint8_t)nibble - (uint8_t)k_usb_kbd_hex_digit_split));
  }
}

/**
 * @brief Push a literal block over SCI8 polled.
 *
 * @param[in] data Buffer to send.
 * @param[in] len  Byte count.
 *
 * @return ra_err_t passthrough from `ra_sci_write_polling`.
 *
 * @pre `data` is non-NULL.
 *
 * @post Bytes have been pushed out the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_sci_write(const uint8_t* data, uint32_t len)
{
  return ra_sci_write_polling((uint8_t)k_usb_kbd_sci_channel, data, len);
}

/**
 * @brief Print "ra8d2 host: device attached vid=0x.. pid=0x..".
 *
 * @param[in] vid USB vendor ID.
 * @param[in] pid USB product ID.
 *
 * @return ra_err_t propagated from SCI helpers.
 *
 * @retval k_ra_ok All chunks queued.
 * @retval other   Underlying SCI failure.
 *
 * @pre SCI8 init already ran.
 *
 * @post One ASCII line is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_print_attach(uint16_t vid, uint16_t pid)
{
  uint8_t hex_vid[k_usb_kbd_hex_chars_u16] = {};
  uint8_t hex_pid[k_usb_kbd_hex_chars_u16] = {};
  usb_kbd_format_hex_u16(vid, hex_vid);
  usb_kbd_format_hex_u16(pid, hex_pid);

  ra_err_t err =
    usb_kbd_sci_write(k_usb_kbd_msg_attach_pre, (uint32_t)(sizeof(k_usb_kbd_msg_attach_pre) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_kbd_sci_write(hex_vid, (uint32_t)k_usb_kbd_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    usb_kbd_sci_write(k_usb_kbd_msg_attach_mid, (uint32_t)(sizeof(k_usb_kbd_msg_attach_mid) - 1U));
  if (err != k_ra_ok) {
    return err;
  }
  err = usb_kbd_sci_write(hex_pid, (uint32_t)k_usb_kbd_hex_chars_u16);
  if (err != k_ra_ok) {
    return err;
  }
  return usb_kbd_sci_write(k_usb_kbd_msg_crlf, (uint32_t)(sizeof(k_usb_kbd_msg_crlf) - 1U));
}

/* =============================================================================
 * Pin / SCI / USB bring-up
 * =============================================================================
 */

/**
 * @brief Route SCI8 J-Link OB CDC pins (PD_02 = TX, PD_03 = RX).
 *
 * @return Error code from the first failing route call, or k_ra_ok.
 *
 * @retval k_ra_ok                     Both pins routed.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success PD_02 and PD_03 are SCI-async (PSEL = 0x04).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_pins_sci_init(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_usb_kbd_pin_sci_tx, k_ra_psel_sci_async, "usb_host_keyboard.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_usb_kbd_pin_sci_rx,
                                 k_ra_psel_sci_async,
                                 "usb_host_keyboard.rxd8");
}

/**
 * @brief Route the USBHS_VBUS sense pin (P4_08, PSEL = 0x14).
 *
 * @return Error code from `ra_pfs_route_peripheral`.
 *
 * @retval k_ra_ok                     P4_08 is now USBHS_VBUS.
 * @retval k_ra_err_gpio_invalid_port  Port index out of range.
 * @retval k_ra_err_gpio_invalid_pin   Pin index out of range.
 * @retval k_ra_err_gpio_conflict      Pin already claimed.
 *
 * @pre IOPORT module is reachable.
 * @pre Caller is single-threaded init context.
 *
 * @post On success P4_08 PFS PSEL = 0x14, PMR = 1.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_pins_hs_init(void)
{
  return ra_pfs_route_peripheral(k_usb_kbd_pin_hs_vbus,
                                 (ra_psel_t)k_usb_kbd_psel_usb_hs,
                                 "usb_host_keyboard.hs_vbus");
}

/**
 * @brief Attach callback -- store the device snapshot, light LED1.
 *
 * @param[in] ctx    Caller context (unused).
 * @param[in] device Pointer to the attach snapshot.
 *
 * @pre Driver was initialised via `ra_usb_hhid_init`.
 *
 * @post `s_state.attached` is true, `s_state.device` carries the
 *       VID/PID/EP info, and LED1 is driven high.
 *
 * @note Invoked from dispatch context; do not call SCI from here.
 *
 * @since 0.1.0
 */
static void usb_kbd_on_attach(void* ctx, const ra_usb_hhid_device_t* device)
{
  (void)ctx;
  if (device == nullptr) {
    return;
  }
  s_state.device   = *device;
  s_state.attached = true;
  (void)ra_board_led_on(k_ra_board_led1);
}

/**
 * @brief Bring CGC + SCI + LEDs + USBHS pins + host-HID up.
 *        Panic-halts on any failure.
 *
 * @since 0.1.0
 */
static void usb_kbd_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    usb_kbd_panic_halt();
  }

  if (usb_kbd_pins_sci_init() != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_usb_kbd_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_usb_kbd_sci_channel, &sci_cfg) != k_ra_ok) {
    usb_kbd_panic_halt();
  }

  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    usb_kbd_panic_halt();
  }

  if (usb_kbd_pins_hs_init() != k_ra_ok) {
    usb_kbd_panic_halt();
  }

  if (ra_usb_hhid_init(k_ra_usb_speed_hs) != k_ra_ok) {
    usb_kbd_panic_halt();
  }
  if (ra_usb_hhid_attach_callback(usb_kbd_on_attach, nullptr) != k_ra_ok) {
    usb_kbd_panic_halt();
  }
}

/* =============================================================================
 * Boot-protocol init + report decode
 * =============================================================================
 */

/**
 * @brief After attach, switch the keyboard to boot protocol + idle 0.
 *
 * @return ra_err_t First non-OK from the underlying class control
 *         transfers, or k_ra_ok.
 *
 * @retval k_ra_ok                Both control transfers issued.
 * @retval k_ra_err_invalid_state Driver lost the device mid-call.
 * @retval other                  Underlying class-control failure.
 *
 * @pre Attach callback has fired.
 *
 * @post Keyboard is in boot protocol with idle = 0 (only report on
 *       change, USB HID 1.11 sec 7.2.4).
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_arm_boot_protocol(void)
{
  ra_err_t err = ra_usb_hhid_set_protocol(k_ra_hhid_proto_boot);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_usb_hhid_set_idle(0U, 0U);
}

/**
 * @brief Test if `keycode` is present anywhere in `report`'s key slots.
 *
 * @param[in] report  Pointer to the 8-byte boot-protocol report.
 * @param[in] keycode Keycode under test.
 *
 * @return `true` if `keycode` appears in slots 2..7, `false` otherwise.
 *
 * @pre `report` is non-NULL and points to >=8 bytes.
 *
 * @post Pure-functional probe; no state mutated.
 *
 * @since 0.1.0
 */
static bool usb_kbd_report_has_key(const uint8_t* report, uint8_t keycode)
{
  for (uint8_t i = k_usb_kbd_report_idx_keys_first; i <= k_usb_kbd_report_idx_keys_last; i++) {
    if (report[i] == keycode) {
      return true;
    }
  }
  return false;
}

/**
 * @brief Decode a keycode + modifier byte to one ASCII byte (or 0).
 *
 * @param[in] keycode HID usage ID.
 * @param[in] modifier Modifier bitmap from the report's first byte.
 *
 * @return ASCII byte, or 0 if the keycode is outside the decode table.
 *
 * @pre None.
 *
 * @post Pure-functional decode.
 *
 * @since 0.1.0
 */
static uint8_t usb_kbd_decode_keycode(uint8_t keycode, uint8_t modifier)
{
  if ((keycode < k_usb_kbd_table_lo) || (keycode > k_usb_kbd_table_hi)) {
    return 0U;
  }
  const uint8_t idx = (uint8_t)(keycode - k_usb_kbd_table_lo);
  const bool    shift =
    (((modifier & k_usb_kbd_mod_lshift) != 0U) || ((modifier & k_usb_kbd_mod_rshift) != 0U));
  if (shift) {
    const uint8_t glyph = k_usb_kbd_ascii_shifted[idx];
    if (glyph != 0U) {
      return glyph;
    }
  }
  return k_usb_kbd_ascii_unshifted[idx];
}

/**
 * @brief Print one decoded keystroke (or the ESC marker) over SCI8.
 *
 * @param[in] glyph ASCII byte to print. 0 means "skip" (undecoded
 *                  keycode); 0x1B means "print ^[ marker instead".
 *
 * @return ra_err_t passthrough from SCI helpers.
 *
 * @retval k_ra_ok Glyph queued (or skipped).
 * @retval other   Underlying SCI failure.
 *
 * @pre SCI8 init already ran.
 *
 * @post One byte (or two for ESC) is in the SCI8 TX FIFO.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_emit_glyph(uint8_t glyph)
{
  if (glyph == 0U) {
    return k_ra_ok;
  }
  if (glyph == k_usb_kbd_glyph_esc) {
    return usb_kbd_sci_write(k_usb_kbd_msg_esc, (uint32_t)(sizeof(k_usb_kbd_msg_esc) - 1U));
  }
  const uint8_t one[1] = {glyph};
  return usb_kbd_sci_write(one, 1U);
}

/**
 * @brief Walk the new report, emit ASCII for keys that just went down,
 *        toggle LED2 per fresh keystroke.
 *
 * @details
 * The boot-protocol report carries up to 6 simultaneous keycodes. We
 * compare against the previous report so a held-down key only emits
 * once. Modifier byte stays in the new report so Shift composes with
 * the keycodes that landed in this same frame.
 *
 * @param[in] prev Previous report (8 bytes) -- used for edge detect.
 * @param[in] curr Current  report (8 bytes).
 *
 * @return ra_err_t First non-OK from the SCI / GPIO helpers, or k_ra_ok.
 *
 * @retval k_ra_ok All keys handled.
 * @retval other   Underlying SCI / GPIO failure.
 *
 * @pre `prev` and `curr` non-NULL with >=8 bytes each.
 *
 * @post Each key that just went down has produced an SCI byte (if its
 *       keycode is in the decode table) and a LED2 toggle.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_handle_report(const uint8_t* prev, const uint8_t* curr)
{
  const uint8_t modifier = curr[k_usb_kbd_report_idx_mod];
  for (uint8_t i = k_usb_kbd_report_idx_keys_first; i <= k_usb_kbd_report_idx_keys_last; i++) {
    const uint8_t keycode = curr[i];
    if (keycode == 0U) {
      continue;
    }
    if (usb_kbd_report_has_key(prev, keycode)) {
      continue;
    }
    /* New keypress: blink LED2 and try to emit ASCII. */
    if (ra_board_led_toggle(k_ra_board_led2) != k_ra_ok) {
      return k_ra_err_gpio_invalid_pin;
    }
    const uint8_t  glyph = usb_kbd_decode_keycode(keycode, modifier);
    const ra_err_t err   = usb_kbd_emit_glyph(glyph);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief One tick of the keyboard polling loop.
 *
 * @details Drains the next interrupt-IN report and feeds it to
 * `usb_kbd_handle_report`. Quietly returns k_ra_ok when the pipe is
 * empty so the main loop just keeps polling.
 *
 * @param[in,out] last_report Mutable 8-byte cache of the previous
 *                            report so the edge-detect logic can spot
 *                            new keypresses.
 *
 * @return ra_err_t First non-OK from the USB / SCI helpers, or k_ra_ok.
 *
 * @retval k_ra_ok One iteration completed.
 * @retval other   Underlying USB / SCI failure.
 *
 * @pre `last_report` is non-NULL and has 8 bytes of storage.
 * @pre Attach callback has fired.
 *
 * @post On success `last_report` matches the report we just consumed.
 *
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t usb_kbd_poll_once(uint8_t* last_report)
{
  uint8_t  report[k_usb_kbd_report_bytes] = {};
  uint16_t got                            = 0U;
  ra_err_t err = ra_usb_hhid_get_input_report(report, k_usb_kbd_report_bytes, &got);
  if (err == k_ra_err_no_data) {
    return k_ra_ok;
  }
  if (err != k_ra_ok) {
    return err;
  }
  if (got < k_usb_kbd_report_bytes) {
    /* Short report -- ignore; boot-protocol mandates 8 bytes. */
    return k_ra_ok;
  }
  err = usb_kbd_handle_report(last_report, report);
  if (err != k_ra_ok) {
    return err;
  }
  for (uint8_t i = 0U; i < k_usb_kbd_report_bytes; i++) {
    last_report[i] = report[i];
  }
  return k_ra_ok;
}

/* =============================================================================
 * Entry point
 * =============================================================================
 */

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief Application entry. Brings up host-HID, switches the attached
 *        keyboard to boot protocol, and pumps decoded keystrokes to
 *        SCI8 forever.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler has copied .data and zeroed .bss.
 * @pre SystemInit has set VTOR, FPU, and priority grouping.
 *
 * @post On clean entry the CPU stays in the get-report -> decode ->
 *       SCI loop forever.
 * @post On any HAL init failure the function halts in WFI.
 *
 * @since 0.1.0
 */
int32_t main(void)
{
  usb_kbd_setup_or_halt();

  ra_isr_globals_enable();

  if (usb_kbd_sci_write(k_usb_kbd_msg_ready, (uint32_t)(sizeof(k_usb_kbd_msg_ready) - 1U)) !=
      k_ra_ok) {
    usb_kbd_panic_halt();
  }

  bool    announced                           = false;
  bool    boot_armed                          = false;
  uint8_t last_report[k_usb_kbd_report_bytes] = {};

  while (1) {
    if (s_state.attached && !announced) {
      const uint16_t vid = s_state.device.vendor_id;
      const uint16_t pid = s_state.device.product_id;
      if (usb_kbd_print_attach(vid, pid) != k_ra_ok) {
        break;
      }
      announced = true;
    }
    if (s_state.attached && !boot_armed) {
      if (usb_kbd_arm_boot_protocol() != k_ra_ok) {
        break;
      }
      boot_armed = true;
    }
    if (s_state.attached && boot_armed) {
      if (usb_kbd_poll_once(last_report) != k_ra_ok) {
        break;
      }
    }
    ra_delay_ms(k_usb_kbd_poll_ms);
  }

  usb_kbd_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
