/**
 * @file examples/ek_ra8d2/hw_validated/hil/keyboard/main.c
 * @brief On-silicon HIL for the on-screen keyboard widget `ra8_keyboard` (#105).
 *
 * @details
 * Drives the real `ra8_keyboard` model with synthetic taps -- the same
 * input-injection pattern as `ereader_input` (#118), but for text entry.
 * It lays the iOS-style key grid into a frame, then "taps" key centres to type
 * the string `Hi 9` -- exercising one-shot SHIFT (the capital `H`), SPACE, and
 * the 123 layer toggle to reach a digit (`9`) -- routing every tap through
 * `ra8_kbd_hit` -> `ra8_kbd_apply` exactly as a finger on the panel would, then
 * commits with ENTER and asserts the query before printing a banner on the
 * SCI8 J-Link OB console:
 *
 *   `kbd: q=Hi 9 commit=1 taps=7 PASS`
 *
 * No panel / SD / touch hardware is needed -- the widget is pure layout +
 * hit-test + text-buffer logic, so the banner is identical on host, ra8_emulator,
 * and silicon. A mismatch halts on a BKPT before the PASS line.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 7 / App] {World: NS}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_keyboard.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_ui.h"

/** @enum kb_consts_t @brief Console / frame knobs (no magic numbers). */
typedef enum : int32_t {
  k_kb_uart_baud = 115200, /**< Console baud.                      */
  k_kb_frame_x   = 0,      /**< Keyboard frame: left.              */
  k_kb_frame_y   = 600,    /**< Keyboard frame: top (lower panel). */
  k_kb_frame_w   = 1024,   /**< Keyboard frame: width.             */
  k_kb_frame_h   = 360,    /**< Keyboard frame: height.            */
  k_kb_half      = 2,      /**< Centre divisor.                    */
  k_kb_dec_ten   = 10,     /**< Decimal radix / small-buf cap.     */
} kb_consts_t;

/** @brief The laid-out keyboard (large -- file-scope, not on the stack). */
static ra8_kbd_layout_t s_kb;

/** @brief Lowercase letters typed (with SHIFT before the first) -> "Hi". */
static const char k_kb_word[] = "hi";
/** @brief Expected query after the SHIFT / SPACE / 123-layer / digit sequence. */
static const char k_kb_expected[] = "Hi 9";

static const uint8_t k_msg_boot[] = "keyboard-hil: boot\r\n";
static const uint8_t k_msg_fail[] = "keyboard-hil: FAIL init\r\n";
static const uint8_t k_msg_lay[]  = "kbd: FAIL layout\r\n";
static const uint8_t k_msg_miss[] = "kbd: FAIL mismatch\r\n";
static const uint8_t k_msg_pre[]  = "kbd: q=";
static const uint8_t k_msg_mid[]  = " commit=";
static const uint8_t k_msg_taps[] = " taps=";
static const uint8_t k_msg_ok[]   = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void kb_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write((msg), (size_t)(len));
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void kb_panic_halt(const uint8_t* msg, uint32_t len)
{
  kb_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer in decimal. */
static void kb_print_uint(uint32_t value)
{
  uint8_t  buf[k_kb_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_kb_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_kb_dec_ten));
    n++;
    value /= (uint32_t)k_kb_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    kb_print(&buf[n - 1U - i], 1U);
  }
}

/** @brief Find the key index of printable @p ch, or k_ra8_kbd_no_hit. */
static uint8_t kb_key_of(char ch)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if ((s_kb.keys[i].kind == k_ra8_kbd_key_char) && (s_kb.keys[i].ch_lower == ch)) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/** @brief Find the first key of a non-char @p kind, or k_ra8_kbd_no_hit. */
static uint8_t kb_key_of_kind(ra8_kbd_key_kind_t kind)
{
  for (uint8_t i = 0U; i < s_kb.count; i++) {
    if (s_kb.keys[i].kind == kind) {
      return i;
    }
  }
  return (uint8_t)k_ra8_kbd_no_hit;
}

/** @brief Tap key @p idx's centre through hit-test + apply; count the tap. */
static void kb_tap(ra8_kbd_text_t* t, uint8_t idx, uint32_t* taps)
{
  const ra8_ui_rect_t* r   = &s_kb.keys[idx].rect;
  const int32_t        cx  = r->x + (r->w / (int32_t)k_kb_half);
  const int32_t        cy  = r->y + (r->h / (int32_t)k_kb_half);
  const uint8_t        hit = ra8_kbd_hit(&s_kb, cx, cy);
  (void)ra8_kbd_apply(t, &s_kb, hit);
  (*taps)++;
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void kb_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    kb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    kb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    kb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_kb_uart_baud) != k_ra8_ok) {
    kb_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Compare two NUL-terminated strings for equality. */
static bool kb_streq(const char* a, const char* b)
{
  for (uint32_t i = 0U; i < (uint32_t)k_ra8_kbd_text_max; i++) {
    if (a[i] != b[i]) {
      return false;
    }
    if (a[i] == '\0') {
      return true;
    }
  }
  return false;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: type a phrase via synthetic taps, verify, print the banner.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The query/commit banner is emitted; the CPU then loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  kb_setup_or_halt();
  ra8_isr_globals_enable();
  kb_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  const ra8_ui_rect_t frame = {.x = k_kb_frame_x,
                               .y = k_kb_frame_y,
                               .w = k_kb_frame_w,
                               .h = k_kb_frame_h};
  if (ra8_kbd_layout_init(&s_kb, &frame) != k_ra8_ok) {
    kb_panic_halt(k_msg_lay, (uint32_t)sizeof(k_msg_lay) - 1U);
  }

  ra8_kbd_text_t t;
  if (ra8_kbd_text_init(&t) != k_ra8_ok) {
    kb_panic_halt(k_msg_lay, (uint32_t)sizeof(k_msg_lay) - 1U);
  }

  /* Type "Hi 9" -- exercises one-shot SHIFT (caps), SPACE, and the 123 layer
   * toggle to reach a digit, then commits with RETURN. */
  uint32_t taps = 0U;
  kb_tap(&t, kb_key_of_kind(k_ra8_kbd_key_shift), &taps); /* -> capitalise next */
  for (uint32_t i = 0U; i < (uint32_t)(sizeof(k_kb_word) - 1U); i++) {
    kb_tap(&t, kb_key_of(k_kb_word[i]), &taps); /* h -> H (one-shot), then i */
  }
  kb_tap(&t, kb_key_of_kind(k_ra8_kbd_key_space), &taps);
  kb_tap(&t, kb_key_of_kind(k_ra8_kbd_key_layer), &taps); /* 123 -> numbers layer */
  kb_tap(&t, kb_key_of('9'), &taps);                      /* digit, now reachable */
  kb_tap(&t, kb_key_of_kind(k_ra8_kbd_key_enter), &taps); /* RETURN commits       */

  if (!kb_streq(t.buf, k_kb_expected) || !t.committed) {
    kb_panic_halt(k_msg_miss, (uint32_t)sizeof(k_msg_miss) - 1U);
  }

  kb_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  kb_print((const uint8_t*)t.buf, (uint32_t)t.len);
  kb_print(k_msg_mid, (uint32_t)sizeof(k_msg_mid) - 1U);
  kb_print_uint((uint32_t)(t.committed ? 1U : 0U));
  kb_print(k_msg_taps, (uint32_t)sizeof(k_msg_taps) - 1U);
  kb_print_uint(taps);
  kb_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
