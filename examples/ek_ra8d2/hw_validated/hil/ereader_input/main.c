/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hw_validated/hil/ereader_input/main.c
 * @brief Headless on-silicon HIL gate for e-reader interaction / hit-test (#118).
 *
 * @details
 * #80 built the e-reader interaction layer -- `ra8_ui` hit-testing + screen-stack
 * navigation -- but no HIL routes an input event through it; the chrome HIL only
 * checks the *render*. This app closes that gap with **synthetic** input (no
 * GT911 touch needed): it builds a representative chrome target set (book cells
 * + a toolbar button), injects a sequence of taps at known coordinates, and
 * asserts each resolves to the expected `ra8_ui` action id (and that off-target
 * taps miss). It then drives the screen stack (open book -> reading, back ->
 * library) and asserts each transition. The result is printed on the SCI8
 * J-Link OB console:
 *
 *   `ui-hil: taps=<N> hits=<M> nav_ok=<0|1> PASS`
 *
 * Pure logic (no peripheral state), so the banner is identical every boot and
 * matches the host / ra8_emulator run.
 *
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
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_ui.h"

/** @enum iu_consts_t @brief Console knobs (no magic numbers). */
typedef enum : uint32_t {
  k_iu_uart_baud = 115200U, /**< Console baud. */
  k_iu_dec_ten   = 10U,     /**< Decimal base. */
} iu_consts_t;

/** @enum iu_action_t @brief Chrome tap-target action ids. */
typedef enum : uint16_t {
  k_iu_act_book0   = 10U, /**< Library book cell 0.       */
  k_iu_act_book1   = 11U, /**< Library book cell 1.       */
  k_iu_act_book2   = 12U, /**< Library book cell 2.       */
  k_iu_act_book3   = 13U, /**< Library book cell 3.       */
  k_iu_act_toolbar = 20U, /**< Status-bar toolbar button. */
} iu_action_t;

/** @enum iu_screen_t @brief Screen ids for the nav stack. */
typedef enum : uint16_t {
  k_iu_screen_library = 1U, /**< Library / home grid. */
  k_iu_screen_reading = 2U, /**< Reading view.        */
} iu_screen_t;

/** @enum iu_miss_pt_t @brief Off-target tap coordinates that must miss. */
typedef enum : int32_t {
  k_iu_miss_gutter_x = 112, /**< Column gutter (between cells 0 and 1). */
  k_iu_miss_gutter_y = 70,  /**< Mid-height of the first cell row.      */
  k_iu_miss_off_x    = 300, /**< Far off-screen x.                      */
  k_iu_miss_off_y    = 400, /**< Far off-screen y.                      */
  k_iu_tap_half      = 2,   /**< Divisor for a rect's centre point.     */
  k_iu_miss_count    = 2,   /**< Number of off-target (miss) taps.      */
} iu_miss_pt_t;

/** @enum iu_rect_t @brief Pixel geometry of the chrome tap-target rects. */
typedef enum : int32_t {
  k_iu_grid_col0_x = 8,   /**< Left column x of the 2x2 book grid.  */
  k_iu_grid_col1_x = 116, /**< Right column x of the 2x2 book grid. */
  k_iu_grid_row0_y = 40,  /**< Top row y of the 2x2 book grid.      */
  k_iu_grid_row1_y = 110, /**< Bottom row y of the 2x2 book grid.   */
  k_iu_cell_w      = 100, /**< Book-cell width.                     */
  k_iu_cell_h      = 60,  /**< Book-cell height.                    */
  k_iu_toolbar_y   = 4,   /**< Toolbar-button y.                    */
  k_iu_toolbar_w   = 60,  /**< Toolbar-button width.                */
  k_iu_toolbar_h   = 28,  /**< Toolbar-button height.               */
} iu_rect_t;

/** @brief Representative chrome targets: a 2x2 book grid + a toolbar button. */
static const ra8_ui_target_t k_iu_targets[] = {
  {.rect      = {.x = k_iu_grid_col0_x, .y = k_iu_grid_row0_y, .w = k_iu_cell_w, .h = k_iu_cell_h},
   .action_id = (uint16_t)k_iu_act_book0},
  {.rect      = {.x = k_iu_grid_col1_x, .y = k_iu_grid_row0_y, .w = k_iu_cell_w, .h = k_iu_cell_h},
   .action_id = (uint16_t)k_iu_act_book1},
  {.rect      = {.x = k_iu_grid_col0_x, .y = k_iu_grid_row1_y, .w = k_iu_cell_w, .h = k_iu_cell_h},
   .action_id = (uint16_t)k_iu_act_book2},
  {.rect      = {.x = k_iu_grid_col1_x, .y = k_iu_grid_row1_y, .w = k_iu_cell_w, .h = k_iu_cell_h},
   .action_id = (uint16_t)k_iu_act_book3},
  {.rect = {.x = k_iu_grid_col0_x, .y = k_iu_toolbar_y, .w = k_iu_toolbar_w, .h = k_iu_toolbar_h},
   .action_id = (uint16_t)k_iu_act_toolbar},
};

static const uint8_t k_msg_boot[]  = "ui-hil: boot\r\n";
static const uint8_t k_msg_fail[]  = "ui-hil: FAIL init\r\n";
static const uint8_t k_msg_pre[]   = "ui-hil: taps=";
static const uint8_t k_msg_hits[]  = " hits=";
static const uint8_t k_msg_nav[]   = " nav_ok=";
static const uint8_t k_msg_pass[]  = " PASS\r\n";
static const uint8_t k_msg_failr[] = " FAIL\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void iu_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void iu_panic_halt(void)
{
  iu_print(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Print a small unsigned integer in decimal. */
static void iu_print_uint(uint32_t value)
{
  uint8_t  buf[k_iu_dec_ten];
  uint32_t n = 0U;
  if (value == 0U) {
    buf[n] = '0';
    n++;
  }
  while ((value > 0U) && (n < (uint32_t)k_iu_dec_ten)) {
    buf[n] = (uint8_t)('0' + (value % (uint32_t)k_iu_dec_ten));
    n++;
    value /= (uint32_t)k_iu_dec_ten;
  }
  for (uint32_t i = 0U; i < n; i++) {
    iu_print(&buf[n - 1U - i], 1U);
  }
}

/**
 * @brief Hit-test a synthetic tap; verify hit-ness + the resolved action.
 *
 * @param[in]     px        Tap x.
 * @param[in]     py        Tap y.
 * @param[in]     want_hit  Expected hit-ness.
 * @param[in]     want_act  Expected action id (when @p want_hit).
 * @param[in,out] hits      Running hit counter (incremented on a real hit).
 * @return true iff the tap resolved exactly as expected.
 */
static bool iu_tap(int32_t px, int32_t py, bool want_hit, uint16_t want_act, uint32_t* hits)
{
  const uint16_t count  = (uint16_t)(sizeof(k_iu_targets) / sizeof(k_iu_targets[0]));
  uint16_t       action = 0U;
  bool           hit    = false;
  if (ra8_ui_hit_test(k_iu_targets, count, px, py, &action, &hit) != k_ra8_ok) {
    return false;
  }
  if (hit) {
    (*hits)++;
  }
  if (hit != want_hit) {
    return false;
  }
  return want_hit ? (action == want_act) : true;
}

/**
 * @brief Drive the screen stack: open book -> reading, back -> library.
 *
 * @return true iff every transition + top-of-stack check matched.
 */
static bool iu_nav_check(void)
{
  ra8_ui_nav_t nav = {};
  uint16_t     top = 0U;
  if (ra8_ui_nav_init(&nav, (uint16_t)k_iu_screen_library) != k_ra8_ok) {
    return false;
  }
  if ((ra8_ui_nav_top(&nav, &top) != k_ra8_ok) || (top != (uint16_t)k_iu_screen_library)) {
    return false;
  }
  /* Open a book: push the reading screen. */
  if (ra8_ui_nav_push(&nav, (uint16_t)k_iu_screen_reading) != k_ra8_ok) {
    return false;
  }
  if ((ra8_ui_nav_top(&nav, &top) != k_ra8_ok) || (top != (uint16_t)k_iu_screen_reading)) {
    return false;
  }
  /* Back: pop reveals (and returns) the library beneath. */
  uint16_t new_top = 0U;
  if ((ra8_ui_nav_pop(&nav, &new_top) != k_ra8_ok) || (new_top != (uint16_t)k_iu_screen_library)) {
    return false;
  }
  /* The root is never popped: a pop at depth 1 must fail and leave the root. */
  if (ra8_ui_nav_pop(&nav, &new_top) == k_ra8_ok) {
    return false;
  }
  return (ra8_ui_nav_top(&nav, &top) == k_ra8_ok) && (top == (uint16_t)k_iu_screen_library);
}

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void iu_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    iu_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    iu_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    iu_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_iu_uart_baud) != k_ra8_ok) {
    iu_panic_halt();
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: inject synthetic taps + nav events, assert, print result.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The hit-test / nav result banner is emitted; the CPU loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  iu_setup_or_halt();
  ra8_isr_globals_enable();
  iu_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  uint32_t taps = 0U;
  uint32_t hits = 0U;
  bool     ok   = true;

  /* Tap each target's centre -> a hit on its action id. */
  const uint16_t ntargets = (uint16_t)(sizeof(k_iu_targets) / sizeof(k_iu_targets[0]));
  for (uint16_t t = 0U; t < ntargets; ++t) {
    const ra8_ui_rect_t* r  = &k_iu_targets[t].rect;
    const int32_t        cx = r->x + (r->w / (int32_t)k_iu_tap_half);
    const int32_t        cy = r->y + (r->h / (int32_t)k_iu_tap_half);
    ok                      = iu_tap(cx, cy, true, k_iu_targets[t].action_id, &hits) && ok;
    taps++;
  }
  /* Off-target taps -> misses (the column gutter, and far off-screen). */
  ok = iu_tap((int32_t)k_iu_miss_gutter_x, (int32_t)k_iu_miss_gutter_y, false, 0U, &hits) && ok;
  taps++;
  ok = iu_tap((int32_t)k_iu_miss_off_x, (int32_t)k_iu_miss_off_y, false, 0U, &hits) && ok;
  taps++;

  const bool nav_ok = iu_nav_check();
  ok                = ok && nav_ok && (hits == (uint32_t)ntargets) &&
                      (taps == (uint32_t)(ntargets + (uint16_t)k_iu_miss_count));

  iu_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  iu_print_uint(taps);
  iu_print(k_msg_hits, (uint32_t)sizeof(k_msg_hits) - 1U);
  iu_print_uint(hits);
  iu_print(k_msg_nav, (uint32_t)sizeof(k_msg_nav) - 1U);
  iu_print_uint(nav_ok ? 1U : 0U);
  if (ok) {
    iu_print(k_msg_pass, (uint32_t)sizeof(k_msg_pass) - 1U);
  } else {
    iu_print(k_msg_failr, (uint32_t)sizeof(k_msg_failr) - 1U);
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
