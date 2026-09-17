/**
 * @file emu_view.c
 * @brief Board-view presentation implementation (see emu_view.h)
 *
 * @details
 * Frame building from live GLCDC state, PPM writing, the status-sidebar
 * snapshot, rotation/unrotation, input routing shared by the window and the
 * headless --click, the panel-descriptor loader, and the console-scrollback
 * + core-control state -- moved verbatim out of the ra8_emulator main
 * translation unit.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "emu_view.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board_console.h"
#include "board_periph.h"
#include "board_periph_sd.h"
#include "board_usb.h"
#include "emu_exc.h"
#include "emu_host_io_internal.h"
#include "emu_mmio.h"
#include "emu_view_surface_internal.h"

/* Touch on the EK-RA8D2 is now modelled end-to-end: the firmware's real
 * ra8_touch_open / ra8_touch_read run unchanged and drive the GoodIX GT911 over
 * ra8_i3c_transfer (the I3C peripheral in legacy I2C mode), which board_periph
 * models as an I2C bus with a GT911 device. ra8_emulator feeds --click / window
 * clicks into that device (board_periph_touch_inject), so a tap returns through
 * the genuine ra8_touch -> I3C -> GT911 path -- there is no function-level stub. */

/* Run-state telemetry the board view shows (updated by the run loop each
 * present): the current PC, the emulation-chunk counter, and whether the run
 * loop is still live (set false once the run parks / faults / exits). */
static uint32_t s_view_pc;
static uint32_t s_view_chunks;
static bool     s_view_running = true;
/* Console scrollback state, Arduino-Serial-Monitor style. s_view_scroll is the
 * offset in lines back from the newest line (0 = the live tail). When
 * s_view_autoscroll is true the view follows the tail (offset pinned to 0);
 * scrolling up pauses autoscroll and the view holds its ABSOLUTE lines as new
 * output arrives (the offset is advanced by each frame's new-line delta, tracked
 * via s_view_log_seen). Scrolling back down to the tail re-enables autoscroll. */
static uint32_t s_view_scroll;
static bool     s_view_autoscroll = true;
static uint32_t s_view_log_seen;
/* Active console tab: which board_console channel the console panel shows. The
 * tab bar (ALL | UART | ITM | SPI | I2C) switches it on a click; switching
 * resets the scrollback to the live tail of the newly selected channel. */
static board_console_ch_t s_view_console_ch = k_board_console_ch_all;

/* Core-control state (#152 dual-core CLI/GUI asks). s_primary_core gates the
 * M85-only instruction seams and relabels telemetry; s_low_power shrinks the
 * run-chunk budget to model the M33's 4:1-slower clock (Model A), and the GUI
 * low-power button + the --low-power flag both drive it. */
static board_primary_core_t s_primary_core = k_core_m85;
static bool                 s_low_power    = false;

/**
 * @brief Snapshot the LED / USB / UART / IRQ / switch / battery / SD state.
 * @details Snapshot the led / usb / uart / irq / switch / battery / sd state; this step is contained within the emu view model and uses bounded caller or module-owned storage.
 * @param[in,out] st Board-status snapshot used to render or report state.
 * @param[in] app_name App name input used by the operation.
 * @pre Arguments satisfy the ranges documented for fill status hw. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_status_hw(board_status_t* st, const char* app_name)
{
  static const char* const k_led_labels[k_overlay_led_count] = {"LED1", "LED2", "LED3"};
  *st                                                        = (board_status_t){};
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    st->leds[i].on    = board_periph_led_level((board_led_id_t)i) != 0U;
    st->leds[i].color = board_periph_led_color_rgb565((board_led_id_t)i);
    st->leds[i].label = k_led_labels[i];
  }
  st->usb_state = board_usb_state_string();
  st->uart_line = board_periph_uart_last_line();
  st->irq_total = board_periph_irq_total();
  st->irq0      = board_periph_irq_count(0U);
  st->irq1      = board_periph_irq_count(1U);
  st->has_touch = board_periph_touch_last(&st->touch_x, &st->touch_y);
  /* User switches are active-low: a held button reads its pin low. */
  st->sw1_pressed = !board_periph_gpio_get_input((uint8_t)k_emu_sw_port, (uint8_t)k_emu_sw1_pin);
  st->sw2_pressed = !board_periph_gpio_get_input((uint8_t)k_emu_sw_port, (uint8_t)k_emu_sw2_pin);
  board_periph_battery_get(&st->battery_soc, &st->battery_charging);
  st->low_power   = s_low_power;
  st->core_is_m33 = (s_primary_core == k_core_m33);
  st->app_name    = app_name;
  board_sd_info(&st->sd_attached, &st->sd_bytes, &st->sd_fat_bits, &st->sd_label);
}

/* @brief Snapshot the tabbed-console metadata and the active scrollback window. */
/**
 * @brief Populate the console tab-bar metadata in @p st.
 *
 * @details
 * Each board_console channel (ALL | UART | ITM | SPI | I2C) is a tab. The
 * overlay needs each tab's name and live line count, plus which tab is active,
 * to draw the bar and hit-test clicks on it.
 *
 * @param[out] st Status block whose `console_ch_*` fields are filled.
 *
 * @pre @p st is non-NULL.
 * @pre `s_view_console_ch` names a valid channel.
 * @post At most @ref k_overlay_console_tabs_max tabs are described.
 * @post `st->console_active_ch` reflects the current channel.
 *
 * @note Not thread-safe; the viewer is single-threaded host-side.
  * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_console_tabs(board_status_t* st)
{
  st->console_ch_count  = (uint32_t)k_board_console_ch_count;
  st->console_active_ch = (uint32_t)s_view_console_ch;
  for (uint32_t c = 0U; c < (uint32_t)k_board_console_ch_count; c++) {
    if (c >= (uint32_t)k_overlay_console_tabs_max) {
      break;
    }
    st->console_ch_name[c]        = board_console_name((board_console_ch_t)c);
    st->console_ch_count_lines[c] = board_console_count((board_console_ch_t)c);
  }
}

/**
 * @brief Advance and clamp the console scroll offset, returning where to read.
 *
 * @details
 * While autoscroll is on the view follows the live tail. While paused, new
 * lines push the tail forward, so the back-offset advances by the same amount
 * to keep the reader's current lines on screen -- capped at the ring depth,
 * because anything older than the ring has already aged out. The result is
 * re-clamped so a shrunken or shallow history cannot scroll past its top.
 *
 * @param[in] avail Lines currently held in the active channel's ring.
 * @param[in] total Lines ever pushed to that channel (monotonic).
 *
 * @return The back-offset to start reading the visible window from.
 *
 * @pre `s_view_log_seen` holds the previous frame's @p total.
 * @pre The active channel is `s_view_console_ch`.
 * @post `s_view_scroll` is <= the maximum scroll for @p avail.
 * @post `s_view_log_seen` is updated to @p total.
 *
 * @note Not thread-safe; the viewer is single-threaded host-side.
  * @retval value The operation-specific console advance scroll value.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_console_advance_scroll(uint32_t avail, uint32_t total)
{
  if (s_view_autoscroll) {
    s_view_scroll = 0U; /* follow the live tail */
  } else if (total > s_view_log_seen) {
    const uint32_t delta = total - s_view_log_seen;
    s_view_scroll += delta;
    if (s_view_scroll > (uint32_t)k_uart_log_max) {
      s_view_scroll = (uint32_t)k_uart_log_max;
    }
  }
  s_view_log_seen = total;
  const uint32_t max_scroll =
    (avail > (uint32_t)k_overlay_console_rows) ? (avail - (uint32_t)k_overlay_console_rows) : 0U;
  if (s_view_scroll > max_scroll) {
    s_view_scroll = max_scroll; /* re-clamp if history shrank or is shallow */
  }
  return s_view_scroll;
}

/**
 * @brief Perform fill status console for the emu view model.
 * @details Perform fill status console for the emu view model; this step is contained within the emu view model and uses bounded caller or module-owned storage.
 * @param[in,out] st Board-status snapshot used to render or report state.
 * @pre Arguments satisfy the ranges documented for fill status console. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_status_console(board_status_t* st)
{
  internal_fill_console_tabs(st);
  /* Console window: copy up to k_overlay_console_rows lines from the active
   * channel's scrollback ring, starting `scroll` lines back from the newest,
   * so the mouse-wheel can page through history. console[0] is the newest
   * visible line (= ring line `scroll`). */
  const uint32_t avail  = board_console_count(s_view_console_ch);
  const uint32_t total  = board_console_total(s_view_console_ch);
  const uint32_t scroll = internal_console_advance_scroll(avail, total);
  uint32_t       rows   = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_console_rows; i++) {
    const char* line = board_console_line(s_view_console_ch, scroll + i);
    if (line == nullptr) {
      break;
    }
    (void)snprintf(st->console[i], (size_t)k_overlay_line_cap, "%s", line);
    rows++;
  }
  st->console_count      = rows;
  st->console_scroll     = scroll;
  st->console_total      = avail;
  st->console_autoscroll = s_view_autoscroll;
  /* Run-state telemetry (set by the run loop before each present). */
  st->pc            = s_view_pc;
  st->chunks        = s_view_chunks;
  st->running       = s_view_running;
  st->mmio_reads    = emu_mmio_reads();
  st->mmio_writes   = emu_mmio_writes();
  st->uart_tx_total = board_periph_uart_tx_total();
}

/**
 * @brief Snapshot the live peripheral state into a board-view status struct.
 *
 * @details
 * Reads the read-only board_periph / board_usb getters -- per-LED level + lit
 * colour, the USB device-state string, the last captured UART line, the NVIC
 * IRQ totals, and the last drained touch point -- so the overlay can render the
 * status sidebar without reaching into any model's internals.
 *
 * @param[out] st        Status struct to fill.
 * @param[in]  app_name  Window / app title to caption the sidebar with.
 * @return Nothing.
  * @pre Arguments satisfy the ranges documented for fill status. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_fill_status(board_status_t* st, const char* app_name)
{
  internal_fill_status_hw(st, app_name);
  internal_fill_status_console(st);
}

/** @brief Map a click in the rotated displayed panel back to native panel coords. */
void unrotate_click(uint16_t  cx,
                    uint16_t  cy,
                    uint16_t  panel_w,
                    uint16_t  panel_h,
                    uint32_t  deg,
                    uint16_t* nx,
                    uint16_t* ny)
{
  if (deg == (uint32_t)k_rotate_90) {
    *nx = cy;
    *ny = (uint16_t)(panel_h - 1U - cx);
  } else if (deg == (uint32_t)k_rotate_180) {
    *nx = (uint16_t)(panel_w - 1U - cx);
    *ny = (uint16_t)(panel_h - 1U - cy);
  } else if (deg == (uint32_t)k_rotate_270) {
    *nx = (uint16_t)(panel_w - 1U - cy);
    *ny = cx;
  } else {
    *nx = cx;
    *ny = cy;
  }
}

/** @brief Toggle a user switch's pressed level (active-low) for an on-screen button. */
void set_switch(board_overlay_btn_t btn, bool pressed)
{
  const uint8_t pin =
    (btn == k_board_overlay_btn_sw2) ? (uint8_t)k_emu_sw2_pin : (uint8_t)k_emu_sw1_pin;
  /* SW1/SW2 are momentary push-buttons wired active-low (internal pull-up): held
   * down drives the pin LOW (input false), released lets it return HIGH (true).
   * Driving the level directly -- instead of toggling -- makes a click behave as a
   * real button (press on mouse-down, release on mouse-up), not a latching switch. */
  board_periph_gpio_set_input((uint8_t)k_emu_sw_port, pin, !pressed);
}

/**
 * @brief Apply a POWER-section click to the fuel-gauge model.
 *
 * @details A click on the battery slider track maps the click column to a 0..100
 * percent (::board_overlay_battery_pct_at) and writes it while preserving the
 * charge state; a click on the CHG toggle flips charging while preserving SOC.
 * Shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] btn    The POWER button hit (battery slider or CHG toggle).
 * @param[in] cx     Click column in composite pixels (for the slider map).
 * @param[in] disp_w Displayed panel width (the sidebar origin).
 */
void apply_battery_click(board_overlay_btn_t btn, uint16_t cx, uint16_t disp_w)
{
  if (btn == k_board_overlay_btn_battery) {
    uint8_t pct = 0U;
    if (board_overlay_battery_pct_at(cx, disp_w, &pct)) {
      bool charging = false;
      board_periph_battery_get(nullptr, &charging);
      board_periph_battery_set(pct, charging);
    }
  } else if (btn == k_board_overlay_btn_batt_chg) {
    uint8_t soc      = 0U;
    bool    charging = false;
    board_periph_battery_get(&soc, &charging);
    board_periph_battery_set(soc, !charging);
  } else if (btn == k_board_overlay_btn_lowpower) {
    /* CORE low-power toggle: flip the M33 4:1-clock model live (same effect as
     * the headless --low-power flag). */
    s_low_power = !s_low_power;
  }
}

/**
 * @brief Route a composite-space click to the right input model.
 *
 * @details An on-screen SW1 / SW2 button toggles that user switch (active-low);
 * any other click is mapped back through @ref unrotate_click and injected as a
 * GT911 touch -- the same path the firmware's real ra8_touch_read drains. This is
 * shared by the live window and the headless @c --click so both behave alike.
 *
 * @param[in] cx         Click column in composite pixels.
 * @param[in] cy         Click row in composite pixels.
 * @param[in] panel_w    Native panel width (for the touch unrotate).
 * @param[in] panel_h    Native panel height (for the touch unrotate).
 * @param[in] disp_w     Displayed panel width (the sidebar origin for buttons).
 * @param[in] rotate_deg Active display rotation.
 * @return The button hit, or ::k_board_overlay_btn_none for a panel touch.
 */
board_overlay_btn_t route_click(uint16_t cx,
                                uint16_t cy,
                                uint16_t panel_w,
                                uint16_t panel_h,
                                uint16_t disp_w,
                                uint32_t rotate_deg)
{
  /* A click on the console tab bar switches the active channel (and takes
   * precedence over the console-body autoscroll toggle below, since the tab row
   * sits inside the console region). Switching resets the scrollback to the live
   * tail of the newly selected channel. */
  uint32_t tab_idx = 0U;
  if (board_overlay_hit_console_tab(cx, cy, disp_w, (uint32_t)k_board_console_ch_count, &tab_idx)) {
    s_view_console_ch = (board_console_ch_t)tab_idx;
    s_view_scroll     = 0U;
    s_view_autoscroll = true;
    s_view_log_seen   = board_console_total(s_view_console_ch);
    return k_board_overlay_btn_console;
  }
  const board_overlay_btn_t btn = board_overlay_hit_button(cx, cy, disp_w);
  if (btn == k_board_overlay_btn_console) {
    /* Toggle the console autoscroll, Arduino-Serial-Monitor style: pause it
     * while reading, or click again to jump back to the live tail. */
    s_view_autoscroll = !s_view_autoscroll;
    if (s_view_autoscroll) {
      s_view_scroll = 0U; /* resume following the newest line */
    }
    return btn;
  }
  if ((btn == k_board_overlay_btn_battery) || (btn == k_board_overlay_btn_batt_chg) ||
      (btn == k_board_overlay_btn_lowpower)) {
    apply_battery_click(btn, cx, disp_w);
    return btn;
  }
  if (btn != k_board_overlay_btn_none) {
    set_switch(btn, true); /* mouse-down -> press; mouse-up releases it (run loop). */
    return btn;
  }
  uint16_t nx = cx;
  uint16_t ny = cy;
  unrotate_click(cx, cy, panel_w, panel_h, rotate_deg, &nx, &ny);
  board_periph_touch_inject(nx, ny);
  return k_board_overlay_btn_none;
}

bool build_composite(uc_engine*                    uc,
                     emu_presentation_workspace_t* presentation,
                     const char*                   app_name)
{
  board_status_t st = {};
  internal_fill_status(&st, app_name);
  return priv_emu_view_surface_build(uc, presentation, &st);
}

/**
 * @brief Trim trailing space/tab/CR/LF in place.
 * @details Trim trailing space/tab/cr/lf in place; this step is contained within the emu view model and uses bounded caller or module-owned storage.
 * @param[in,out] s Module state instance processed by the operation.
 * @pre Arguments satisfy the ranges documented for panel rstrip. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the emu view model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panel_rstrip(char* s)
{
  size_t n = strlen(s);
  while (n > 0U) {
    const char c = s[n - 1U];
    if ((c != ' ') && (c != '\t') && (c != '\r') && (c != '\n')) {
      break;
    }
    s[--n] = '\0';
  }
}

/**
 * @brief Split one config line into a trimmed `key` / `value` pair, in place.
 *
 * @details
 * Skips leading blanks, ignores comment and blank lines, splits on the first
 * `=`, then right-trims both halves. The line buffer is modified: the `=`
 * becomes the key's terminator and @p key / @p val point into it.
 *
 * @param[in,out] line Mutable line buffer from the config file.
 * @param[out]    key  Receives a pointer to the trimmed key.
 * @param[out]    val  Receives a pointer to the trimmed value.
 *
 * @return True when a `key=value` pair was found, false for a blank, comment,
 *         or `=`-less line the caller should skip.
 *
 * @pre @p line, @p key and @p val are non-NULL.
 * @pre @p line is NUL-terminated and writable.
 * @post On true, both `*key` and `*val` point inside @p line.
 * @post On false, `*key` and `*val` are untouched.
 *
 * @note Not thread-safe; the viewer is single-threaded host-side.
  * @retval true The panel split kv condition holds or completed successfully; false otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_panel_split_kv(char* line, char** key, char** val)
{
  char* p = line;
  while ((*p == ' ') || (*p == '\t')) {
    p++;
  }
  if ((*p == '#') || (*p == '\0') || (*p == '\n') || (*p == '\r')) {
    return false;
  }
  char* eq = strchr(p, '=');
  if (eq == nullptr) {
    return false;
  }
  *eq     = '\0';
  char* v = eq + 1;
  while ((*v == ' ') || (*v == '\t')) {
    v++;
  }
  internal_panel_rstrip(p);
  internal_panel_rstrip(v);
  *key = p;
  *val = v;
  return true;
}

/**
 * @brief Copy the panel `name` value into @p out, stripping optional quotes.
 *
 * @param[out] out Panel description receiving the name.
 * @param[in]  val Raw value text, optionally wrapped in double quotes.
 *
 * @pre @p out and @p val are non-NULL.
 * @pre @p val is NUL-terminated.
 * @post `out->name` is NUL-terminated and never overruns its buffer.
 * @post A value longer than the field is truncated rather than rejected.
 *
 * @note Not thread-safe; the viewer is single-threaded host-side.
  * @details Copy the panel `name` value into @p out, stripping optional quotes; this step is contained within the emu view model and uses bounded caller or module-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_panel_set_name(board_panel_t* out, const char* val)
{
  const char* s = val;
  size_t      n = strlen(s);
  if ((n >= 2U) && (s[0] == '"') && (s[n - 1U] == '"')) {
    s += 1;
    n -= 2U;
  }
  if (n >= sizeof(out->name)) {
    n = sizeof(out->name) - 1U;
  }
  (void)memcpy(out->name, s, n);
  out->name[n] = '\0';
}

/**
 * @brief Apply one recognised `key=value` pair to @p out; unknown keys are ignored.
 *
 * @param[in,out] out Panel description being built.
 * @param[in]     key Trimmed key text.
 * @param[in]     val Trimmed value text.
 *
 * @pre @p out, @p key and @p val are non-NULL.
 * @pre @p key and @p val are NUL-terminated.
 * @post Exactly one field of @p out is written, or none for an unknown key.
 * @post Unknown keys are ignored so newer configs stay loadable.
 *
 * @note Not thread-safe; the viewer is single-threaded host-side.
  * @details Apply one recognised `key=value` pair to @p out; unknown keys are ignored; this step is contained within the emu view model and uses bounded caller or module-owned storage.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_panel_apply_kv(board_panel_t* out, const char* key, const char* val)
{
  if (strncmp(key, "width", sizeof("width")) == 0) {
    out->width = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
  } else if (strncmp(key, "height", sizeof("height")) == 0) {
    out->height = (uint16_t)strtol(val, nullptr, (int)k_strtol_base10);
  } else if (strncmp(key, "name", sizeof("name")) == 0) {
    internal_panel_set_name(out, val);
  } else {
    /* Unknown key: ignored on purpose. */
  }
}

/**
 * @brief Parse bounded panel-config text already loaded into memory.
 * @details Splits newline-delimited records in place and applies recognised key/value pairs.
 * @param[in,out] text Writable NUL-terminated config text.
 * @param[in,out] out Descriptor receiving recognised values.
 * @return True when every input line respected the configured bound.
 * @retval true All lines were bounded and parsed or ignored.
 * @retval false At least one line exceeded k_panel_line_max.
 * @pre @p text is non-null, writable, and NUL-terminated.
 * @pre @p out is non-null and initialised by the caller.
 * @post Newline separators inspected by the parser are replaced with NUL.
 * @post Recognised keys update @p out in input order.
 * @note Not thread-safe; the viewer is single-threaded host-side.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_parse_panel(char* text, board_panel_t* out)
{
  char* line = text;
  while (*line != '\0') {
    char* const end = strchr(line, '\n');
    if (end != nullptr) {
      *end = '\0';
    }
    if (strlen(line) >= (size_t)k_panel_line_max) {
      return false;
    }
    char* key = nullptr;
    char* val = nullptr;
    if (internal_panel_split_kv(line, &key, &val)) {
      internal_panel_apply_kv(out, key, val);
    }
    if (end == nullptr) {
      break;
    }
    line = end + 1;
  }
  return true;
}

/* See the public header for the documented contract. */
bool load_panel(const char* path, board_panel_t* out)
{
  enum : size_t {
    k_panel_file_cap = 4096U, /**< Maximum accepted panel-description byte count. */
  };
  if ((path == nullptr) || (out == nullptr)) {
    return false;
  }
  (void)memset(out, 0, sizeof(*out));
  emu_io_file_t file = {.fd = -1, .size = 0};
  if ((priv_emu_io_open_read(path, &file).status != k_emu_io_ok) || (file.size <= 0) ||
      ((uint64_t)file.size > k_panel_file_cap)) {
    if (file.fd >= 0) {
      (void)priv_emu_io_close(&file);
    }
    (void)priv_emu_io_errf("ra8_emulator: cannot open panel config %s\n", path);
    return false;
  }
  const size_t length = (size_t)file.size;
  char         config[k_panel_file_cap + 1U];
  const bool   read_ok = priv_emu_io_read_exact(file.fd, config, length).status == k_emu_io_ok;
  config[length]       = '\0';
  (void)priv_emu_io_close(&file);
  const bool valid = read_ok && internal_parse_panel(config, out) && (out->width != 0U) &&
                     (out->width <= (uint16_t)k_panel_dim_max) && (out->height != 0U) &&
                     (out->height <= (uint16_t)k_panel_dim_max);
  if (!valid) {
    (void)priv_emu_io_errf("ra8_emulator: panel %s: width/height missing or out of range\n", path);
    return false;
  }
  return true;
}

/** @brief Implementation of `emu_view_publish()` -- run-loop telemetry. */
void emu_view_publish(uint32_t pc, uint32_t chunks)
{
  s_view_pc     = pc;
  s_view_chunks = chunks;
}

/** @brief Implementation of `emu_view_mark_stopped()` -- final held frame. */
void emu_view_mark_stopped(uint32_t pc)
{
  s_view_running = false;
  s_view_pc      = pc;
}

/** @brief Implementation of `emu_view_wheel()` -- scrollback paging. */
void emu_view_wheel(int32_t notches)
{
  if (notches > 0) {
    s_view_autoscroll = false; /* user scrolled up -> pause the live follow */
    s_view_scroll += (uint32_t)notches;
  } else if (notches < 0) {
    const uint32_t dec = (uint32_t)(-notches);
    if (s_view_scroll > dec) {
      s_view_scroll -= dec;
    } else {
      s_view_scroll     = 0U;
      s_view_autoscroll = true; /* scrolled back to the tail -> resume follow */
    }
  }
}

/** @brief Implementation of `emu_view_select_console_tab()` -- tab switch. */
void emu_view_select_console_tab(uint32_t tab_idx)
{
  s_view_console_ch = (board_console_ch_t)tab_idx;
  s_view_scroll     = 0U;
  s_view_autoscroll = true;
  s_view_log_seen   = board_console_total(s_view_console_ch);
}

/** @brief Implementation of `emu_view_reset_console()` -- warm-reboot view reset. */
void emu_view_reset_console(void)
{
  s_view_console_ch = k_board_console_ch_all;
  s_view_scroll     = 0U;
  s_view_autoscroll = true;
  s_view_log_seen   = 0U;
}

/** @brief Implementation of `emu_primary_core()` -- plain state read. */
board_primary_core_t emu_primary_core(void)
{
  return s_primary_core;
}

/** @brief Implementation of `emu_set_primary_core()` -- plain state write. */
void emu_set_primary_core(board_primary_core_t core)
{
  s_primary_core = core;
}

/** @brief Implementation of `emu_low_power()` -- plain state read. */
bool emu_low_power(void)
{
  return s_low_power;
}

/** @brief Implementation of `emu_set_low_power()` -- plain state write. */
void emu_set_low_power(bool on)
{
  s_low_power = on;
}
