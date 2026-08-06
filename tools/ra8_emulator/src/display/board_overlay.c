/**
 * @file board_overlay.c
 * @brief Composite board-view renderer implementation (see board_overlay.h)
 *
 * @details
 * Renders the panel framebuffer plus a status sidebar into one RGB565 buffer.
 * Text is drawn with an embedded 5x7 column-major ASCII font (printable range
 * 0x20..0x7E); each glyph is five bytes, one per column, with the low seven
 * bits giving the rows top-to-bottom. Keeping the whole composite in a plain
 * pixel buffer is deliberate: the macOS window and the @c --ppm snapshot show
 * the identical bytes, so the overlay is verifiable headlessly.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_overlay.h"

#include <stdint.h>
#include <stdio.h>

#include "board_overlay_internal.h"

/** @brief Draw one LED indicator dot (filled when on) plus its caption. */
static void
draw_led(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_led_status_t* led)
{
  enum : int32_t {
    k_dot = 18, /**< LED dot side in pixels. */
  };
  fill_rect(out, w, h, x - 1, y - 1, k_dot + 2, k_dot + 2, (uint16_t)k_ovl_led_ring);
  const uint16_t fill = led->on ? led->color : (uint16_t)k_ovl_led_off;
  fill_rect(out, w, h, x, y, k_dot, k_dot, fill);
  draw_text(out,
            w,
            h,
            x + k_dot + 8,
            y + k_led_label_dy,
            (led->label != nullptr) ? led->label : "LED",
            (uint16_t)k_ovl_text,
            1);
}

/** @brief Paint the three LED indicators in a row; returns the next free y. */
static int32_t
draw_leds(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_status_t* st)
{
  const int32_t row_y = section_head(out, w, h, x, y, "LEDS");
  for (uint32_t i = 0U; i < (uint32_t)k_overlay_led_count; i++) {
    draw_led(out, w, h, x + ((int32_t)i * k_led_col_dx), row_y, &st->leds[i]);
  }
  return row_y + k_led_row_h;
}

/** @brief Paint the run-stats block (PC / chunks / MMIO / state); next free y. */
static int32_t draw_run_stats(uint16_t*             out,
                              uint16_t              w,
                              uint16_t              h,
                              int32_t               x,
                              int32_t               y,
                              const board_status_t* st)
{
  enum : uint8_t {
    k_run_row_cap = 64U, /**< Widest RUN row ("<state>   chunk <n>"), plus slack. */
  };
  char    buf[k_run_row_cap];
  int32_t cy = section_head(out, w, h, x, y, "RUN");
  (void)snprintf(buf, sizeof(buf), "0x%08X", st->pc);
  cy = kv_row(out, w, h, x, cy, "pc", buf, (uint16_t)k_ovl_heading);
  (void)snprintf(buf, sizeof(buf), "%s   chunk %u", st->running ? "RUNNING" : "parked", st->chunks);
  cy =
    kv_row(out, w, h, x, cy, "state", buf, st->running ? (uint16_t)k_ovl_ok : (uint16_t)k_ovl_dim);
  (void)snprintf(buf, sizeof(buf), "%u rd / %u wr", st->mmio_reads, st->mmio_writes);
  cy = kv_row(out, w, h, x, cy, "mmio", buf, (uint16_t)k_ovl_text);
  return cy;
}

/** @brief Paint the I/O block (USB / IRQ / touch / SD); returns next free y. */
static int32_t
draw_io_block(uint16_t* out, uint16_t w, uint16_t h, int32_t x, int32_t y, const board_status_t* st)
{
  enum : uint8_t {
    k_io_row_cap = 96U, /**< Widest I/O row (USB/IRQ/touch/SD summary), plus slack. */
  };
  char    buf[k_io_row_cap];
  int32_t cy = section_head(out, w, h, x, y, "I/O");
  cy         = kv_row(out,
              w,
              h,
              x,
              cy,
              "usb",
              (st->usb_state != nullptr) ? st->usb_state : "-",
              (uint16_t)k_ovl_text);
  (void)
    snprintf(buf, sizeof(buf), "%u total  IRQ0 x%u  IRQ1 x%u", st->irq_total, st->irq0, st->irq1);
  cy = kv_row(out, w, h, x, cy, "irq", buf, (uint16_t)k_ovl_text);
  if (st->has_touch) {
    (void)snprintf(buf, sizeof(buf), "%u, %u", st->touch_x, st->touch_y);
  } else {
    (void)snprintf(buf, sizeof(buf), "-");
  }
  cy                        = kv_row(out, w, h, x, cy, "touch", buf, (uint16_t)k_ovl_text);
  const bool          sd_gb = (st->sd_bytes >= (k_sd_unit_div * k_sd_unit_div * k_sd_unit_div));
  const unsigned long sd_sz =
    sd_gb ? (unsigned long)(st->sd_bytes / (k_sd_unit_div * k_sd_unit_div * k_sd_unit_div))
          : (unsigned long)(st->sd_bytes / (k_sd_unit_div * k_sd_unit_div));
  const char* sd_u = sd_gb ? "GB" : "MB";
  if (!st->sd_attached) {
    (void)snprintf(buf, sizeof(buf), "-");
  } else if (st->sd_fat_bits != 0U) {
    (void)snprintf(buf,
                   sizeof(buf),
                   "%lu %s FAT%u %s",
                   sd_sz,
                   sd_u,
                   (unsigned)st->sd_fat_bits,
                   (st->sd_label != nullptr) ? st->sd_label : "");
  } else {
    (void)snprintf(buf, sizeof(buf), "%lu %s (image)", sd_sz, sd_u);
  }
  cy = kv_row(out, w, h, x, cy, "sd", buf, (uint16_t)k_ovl_text);
  return cy;
}

/**
 * @brief Tabs drawn per row for a wrapping tab grid of @p count cells.
 *
 * @details The tab bar wraps: each row holds as many equal-width cells as fit at
 * the minimum cell pitch ::k_con_tab_min_w, so a wide panel packs more lanes per
 * row and a narrow one fewer. The result is clamped to [1, @p count] so a single
 * lane still gets a full-width tab and the grid never claims more cells per row
 * than exist. Both ::console_tab_rect and ::console_tab_row_count build on this.
 *
 * @param[in] panel_w Console panel width in pixels.
 * @param[in] count   Total tab count (>= 1 expected; 0 is treated as 1).
 * @return Tabs per row, in [1, max(1, @p count)].
 * @retval 1 The panel is narrower than one minimum cell, or @p count is 0/1.
 * @pre @p panel_w is the live console panel width.
 * @pre @p count is the live channel count.
 * @post The result is >= 1 (never zero -- safe as a divisor).
 * @post The result is <= max(1, @p count).
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static uint32_t console_tabs_per_row(int32_t panel_w, uint32_t count)
{
  if (count == 0U) {
    return 1U;
  }
  int32_t per = panel_w / (int32_t)k_con_tab_min_w;
  if (per < 1) {
    per = 1;
  }
  if ((uint32_t)per > count) {
    per = (int32_t)count;
  }
  return (uint32_t)per;
}

/**
 * @brief Number of rows a wrapping tab grid of @p count cells occupies.
 *
 * @details Ceiling-divides @p count by ::console_tabs_per_row, so the tab bar is
 * exactly tall enough to show every lane. ::draw_console and the hit-test use it
 * to size the tab band and leave the rest of the panel for the scrolling body.
 *
 * @param[in] panel_w Console panel width in pixels.
 * @param[in] count   Total tab count.
 * @return Row count, in [1, @p count] for @p count >= 1; 0 when @p count is 0.
 * @retval 0 @p count is 0 (no tabs to draw).
 * @pre @p panel_w is the live console panel width.
 * @pre @p count is the live channel count.
 * @post The result is 0 only when @p count is 0.
 * @post rows * tabs-per-row >= @p count (every cell has a slot).
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static uint32_t console_tab_row_count(int32_t panel_w, uint32_t count)
{
  if (count == 0U) {
    return 0U;
  }
  const uint32_t per_row = console_tabs_per_row(panel_w, count);
  return (count + per_row - 1U) / per_row;
}

/**
 * @brief Compute one console tab's cell rectangle in the wrapping grid.
 *
 * @details The bar wraps into ::console_tabs_per_row columns; cell @p idx lands at
 * row @c idx/per_row, column @c idx%per_row. Each row is ::k_con_tab_h tall and
 * each cell one ::k_con_tab_gap narrower/shorter than its pitch. Both
 * ::draw_console_tabs and ::board_overlay_hit_console_tab derive their geometry
 * here so the drawn tab and its click hit-box stay in lock-step.
 *
 * @param[in]  panel_x Console panel left edge (sidebar left + padding).
 * @param[in]  panel_w Console panel width in pixels.
 * @param[in]  idx     Tab index (0 .. @p count - 1).
 * @param[in]  count   Total tab count (clamped to >= 1 internally).
 * @param[out] r       Receives the cell rect {x, y, w, h}; 4-int array.
 * @return Nothing.
 * @pre @p count is the live channel count (0 is treated as 1).
 * @pre @p r is a non-NULL array of at least 4 int32_t.
 * @post @p r[0]/r[1] are within the console panel band.
 * @post @p r[2]/r[3] (w/h) are >= 0 (clamped, never negative).
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void
console_tab_rect(int32_t panel_x, int32_t panel_w, uint32_t idx, uint32_t count, int32_t* r)
{
  if (r == nullptr) {
    return;
  }
  const uint32_t per_row = console_tabs_per_row(panel_w, count);
  const uint32_t row     = idx / per_row;
  const uint32_t col     = idx % per_row;
  const int32_t  pitch   = panel_w / (int32_t)per_row;
  r[0]                   = panel_x + ((int32_t)col * pitch);
  r[1]                   = (int32_t)k_con_y + ((int32_t)row * (int32_t)k_con_tab_h);
  r[2]                   = pitch - (int32_t)k_con_tab_gap;
  r[3]                   = (int32_t)k_con_tab_h - (int32_t)k_con_tab_gap;
  if (r[2] < 0) {
    r[2] = 0;
  }
  if (r[3] < 0) {
    r[3] = 0;
  }
}

/**
 * @brief Draw the wrapping console tab bar at the top of the console panel.
 *
 * @details One equal-width cell per channel (ALL | UART | ITM | SPI | I2C | CAN |
 * ...), wrapped across as many rows as the panel width needs and captioned
 * "NAME n" where n is the channel's live line count. The active tab is filled
 * bright with white text; an inactive tab with traffic is dim, and an empty
 * channel (count 0) is dimmer still so it reads as "no data yet". Geometry comes
 * from ::console_tab_rect so the drawn cells line up with the click hit-test.
 *
 * @param[out] out     Composite buffer.
 * @param[in]  w       Composite width in pixels.
 * @param[in]  h       Composite height in pixels.
 * @param[in]  panel_x Console panel left edge.
 * @param[in]  panel_w Console panel width in pixels.
 * @param[in]  st      Live status snapshot (tab names / counts / active index).
 * @return Nothing.
 * @pre @p st is non-NULL.
 * @pre @p out points at a @p w by @p h composite buffer.
 * @post Up to ::k_overlay_console_tabs_max tab cells are painted.
 * @post The console body area below the bar is left untouched.
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
static void draw_console_tabs(uint16_t*             out,
                              uint16_t              w,
                              uint16_t              h,
                              int32_t               panel_x,
                              int32_t               panel_w,
                              const board_status_t* st)
{
  if (st == nullptr) {
    return;
  }
  uint32_t count = st->console_ch_count;
  if (count > (uint32_t)k_overlay_console_tabs_max) {
    count = (uint32_t)k_overlay_console_tabs_max;
  }
  for (uint32_t i = 0U; i < count; i++) {
    int32_t rect[4] = {0, 0, 0, 0};
    console_tab_rect(panel_x, panel_w, i, count, rect);
    const bool active = (i == st->console_active_ch);
    const bool empty  = (st->console_ch_count_lines[i] == 0U);
    uint16_t   bg     = (uint16_t)k_ovl_tab_off_bg;
    uint16_t   txt    = (uint16_t)k_ovl_tab_off_txt;
    if (active) {
      bg  = (uint16_t)k_ovl_tab_on_bg;
      txt = (uint16_t)k_ovl_tab_on_txt;
    } else if (empty) {
      txt = (uint16_t)k_ovl_tab_empty_txt;
    }
    fill_rect(out, w, h, rect[0], rect[1], rect[2], rect[3], bg);
    enum : uint8_t {
      k_tab_caption_cap = 24U, /**< Console tab caption: "<name> <line count>". */
    };
    char        cap[k_tab_caption_cap];
    const char* name = (st->console_ch_name[i] != nullptr) ? st->console_ch_name[i] : "?";
    (void)snprintf(cap, sizeof(cap), "%s %u", name, st->console_ch_count_lines[i]);
    draw_text(out,
              w,
              h,
              rect[0] + (int32_t)k_con_tab_txt,
              rect[1] + (int32_t)k_con_tab_cap_y,
              cap,
              txt,
              1);
  }
}

/* @brief Paint the tabbed console panel (newest line at the bottom). */
/**
 * @brief Draw the console heading line and its rule.
 *
 * @details
 * The heading doubles as the scroll indicator: paused shows how far back the
 * held view sits and how to resume, live shows the byte counter.
 *
 * @param[out] out Framebuffer to draw into.
 * @param[in]  w   Framebuffer width, pixels.
 * @param[in]  h   Framebuffer height, pixels.
 * @param[in]  x   Left edge of the sidebar panel.
 * @param[in]  st  Board status carrying the console counters.
 *
 * @pre @p out covers @p w by @p h pixels and @p st is non-NULL.
 * @pre The sidebar geometry constants fit inside @p w.
 * @post The heading colour distinguishes paused (amber) from live.
 * @post A rule is drawn directly under the heading.
 *
 * @note Not thread-safe; the overlay is drawn from one thread.
 */
static void
draw_console_heading(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  enum : uint8_t {
    k_console_head_cap = 80U, /**< Widest heading (the PAUSED variant), plus slack. */
  };
  char     buf[k_console_head_cap];
  uint16_t head_col;
  if (!st->console_autoscroll) {
    /* Paused: amber heading + how far back the held view sits and how to resume. */
    (void)snprintf(buf,
                   sizeof(buf),
                   "CONSOLE  PAUSED -%u/%u  click=follow",
                   st->console_scroll,
                   st->console_total);
    head_col = (uint16_t)k_ovl_amber;
  } else {
    (void)snprintf(buf, sizeof(buf), "CONSOLE  LIVE  (UART %u B, wheel=scroll)", st->uart_tx_total);
    head_col = (uint16_t)k_ovl_ok;
  }
  draw_text(out, w, h, x, (int32_t)k_con_head_y, buf, head_col, 1);
  fill_rect(out,
            w,
            h,
            x,
            (int32_t)k_con_head_y + (int32_t)k_rule_dy,
            (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x),
            1,
            (uint16_t)k_ovl_rule);
}

/**
 * @brief Draw the scrolling console body below the tab bar.
 *
 * @details
 * Lines are drawn bottom-up so the console scrolls like a terminal:
 * `console[0]` is the newest line and lands on the bottom row, highlighted.
 *
 * @param[out] out     Framebuffer to draw into.
 * @param[in]  w       Framebuffer width, pixels.
 * @param[in]  h       Framebuffer height, pixels.
 * @param[in]  panel_x Left edge of the console panel.
 * @param[in]  body_y  Top of the body area, below the tab bar.
 * @param[in]  body_h  Height of the body area, pixels.
 * @param[in]  st      Board status carrying the visible lines.
 *
 * @pre @p out covers @p w by @p h pixels and @p st is non-NULL.
 * @pre @p body_h is at least one line high.
 * @post At most as many lines are drawn as the body height allows.
 * @post No line is drawn outside the body area.
 *
 * @note Not thread-safe; the overlay is drawn from one thread.
 */
static void draw_console_body(uint16_t*             out,
                              uint16_t              w,
                              uint16_t              h,
                              int32_t               panel_x,
                              int32_t               body_y,
                              int32_t               body_h,
                              const board_status_t* st)
{
  const int32_t max_rows = (body_h - (2 * (int32_t)k_con_pad)) / (int32_t)k_con_line_h;
  int32_t       rows     = (int32_t)st->console_count;
  if (rows > max_rows) {
    rows = max_rows;
  }
  for (int32_t i = 0; i < rows; i++) {
    /* Bottom row (i=0 from the bottom) is console[0], the newest line. */
    const int32_t  ly  = body_y + body_h - (int32_t)k_con_pad - ((i + 1) * (int32_t)k_con_line_h);
    const uint16_t col = (i == 0) ? (uint16_t)k_ovl_console_new : (uint16_t)k_ovl_console_txt;
    draw_text(out, w, h, panel_x + (int32_t)k_con_pad, ly, st->console[i], col, 1);
  }
}

static void draw_console(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  draw_console_heading(out, w, h, x, st);
  const int32_t panel_x = x;
  const int32_t panel_w = (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x);
  const int32_t panel_h = (int32_t)h - (int32_t)k_con_y - (int32_t)k_con_bottom;
  if (panel_h < (int32_t)k_con_line_h) {
    return;
  }
  fill_rect(out, w, h, panel_x, (int32_t)k_con_y, panel_w, panel_h, (uint16_t)k_ovl_console_bg);
  /* The tab bar wraps into as many rows as the channel count needs; the scrolling
   * body fills whatever is left below it. */
  draw_console_tabs(out, w, h, panel_x, panel_w, st);
  uint32_t tab_count = st->console_ch_count;
  if (tab_count > (uint32_t)k_overlay_console_tabs_max) {
    tab_count = (uint32_t)k_overlay_console_tabs_max;
  }
  const int32_t tab_rows = (int32_t)console_tab_row_count(panel_w, tab_count);
  const int32_t tab_band = tab_rows * (int32_t)k_con_tab_h;
  const int32_t body_y   = (int32_t)k_con_y + tab_band;
  const int32_t body_h   = panel_h - tab_band;
  if (body_h < (int32_t)k_con_line_h) {
    return;
  }
  /* Fit as many lines as the body height allows; show the newest at the bottom
   * so the console scrolls upward like a terminal. console[0] is the newest. */
  draw_console_body(out, w, h, panel_x, body_y, body_h, st);
}

/** @brief Draw one labelled push-button face at @p x (green when @p pressed). */
static void
draw_button(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const char* label, bool pressed)
{
  const uint16_t face = pressed ? (uint16_t)k_ovl_btn_down : (uint16_t)k_ovl_btn_up;
  fill_rect(out,
            w,
            h,
            x - 1,
            (int32_t)k_btn_y - 1,
            (int32_t)k_btn_w + 2,
            (int32_t)k_btn_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out, w, h, x, (int32_t)k_btn_y, (int32_t)k_btn_w, (int32_t)k_btn_h, face);
  draw_text(out,
            w,
            h,
            x + (int32_t)k_btn_label_x,
            (int32_t)k_btn_y + (int32_t)k_btn_label_y,
            label,
            (uint16_t)k_ovl_btn_label,
            2);
}

/** @brief Paint the "BUTTONS" heading and the clickable SW1 / SW2 buttons. */
static void draw_buttons(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  (void)section_head(out, w, h, x, (int32_t)k_btn_head_y, "BUTTONS  (click)");
  draw_button(out, w, h, x, "SW1", st->sw1_pressed);
  draw_button(out, w, h, x + (int32_t)k_btn_w + (int32_t)k_btn_gap, "SW2", st->sw2_pressed);
}

/** @brief Clamp a SOC value to 0..100 for display. */
static uint8_t battery_clamp(uint8_t soc)
{
  return (soc > (uint8_t)k_pwr_soc_full) ? (uint8_t)k_pwr_soc_full : soc;
}

/** @brief Pick the battery gauge fill colour: green charging, else red/amber/green by level. */
static uint16_t battery_fill_color(uint8_t soc, bool charging)
{
  if (charging) {
    return (uint16_t)k_ovl_ok; /* charging always reads green. */
  }
  if (soc <= (uint8_t)k_pwr_soc_low) {
    return (uint16_t)k_ovl_red;
  }
  if (soc <= (uint8_t)k_pwr_soc_mid) {
    return (uint16_t)k_ovl_amber;
  }
  return (uint16_t)k_ovl_ok;
}

/**
 * @brief Paint the POWER section: a drag-to-set battery slider and a CHG toggle.
 *
 * @details The slider track is a bordered bar whose left portion is filled to the
 * SOC fraction in the level colour (red <=20%, amber <=50%, green above, or green
 * while charging); the "NN%" reading is drawn over it. To the right, a CHG button
 * shows the charge state -- green "CHG +" while charging, dim "BATT" on battery.
 * The geometry matches ::board_overlay_hit_button and
 * ::board_overlay_battery_pct_at so a click drags the percent or toggles charge.
 *
 * @param[out] out Composite buffer.
 * @param[in]  w   Composite width in pixels.
 * @param[in]  h   Composite height in pixels.
 * @param[in]  x   Section text origin (sidebar left + padding).
 * @param[in]  st  Live status snapshot (battery_soc / battery_charging).
 */
static void draw_power(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  (void)section_head(out, w, h, x, (int32_t)k_pwr_head_y, "POWER  (drag)");
  const uint8_t  soc    = battery_clamp(st->battery_soc);
  const int32_t  fill_w = ((int32_t)k_pwr_track_w * (int32_t)soc) / (int32_t)k_pwr_soc_full;
  const uint16_t fill   = battery_fill_color(soc, st->battery_charging);
  /* Slider track: border, dark bed, then the SOC-proportional level fill. */
  fill_rect(out,
            w,
            h,
            x - 1,
            (int32_t)k_pwr_y - 1,
            (int32_t)k_pwr_track_w + 2,
            (int32_t)k_pwr_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out,
            w,
            h,
            x,
            (int32_t)k_pwr_y,
            (int32_t)k_pwr_track_w,
            (int32_t)k_pwr_h,
            (uint16_t)k_ovl_btn_up);
  fill_rect(out, w, h, x, (int32_t)k_pwr_y, fill_w, (int32_t)k_pwr_h, fill);
  char buf[16];
  (void)snprintf(buf, sizeof(buf), "%u%%", (unsigned)soc);
  draw_text(out,
            w,
            h,
            x + (int32_t)k_btn_label_x,
            (int32_t)k_pwr_y + (int32_t)k_pwr_label_y,
            buf,
            (uint16_t)k_ovl_heading,
            1);
  /* CHG toggle: green "CHG +" while charging, dim "BATT" on battery. */
  const int32_t  cx   = x + (int32_t)k_pwr_chg_off;
  const uint16_t face = st->battery_charging ? (uint16_t)k_ovl_btn_down : (uint16_t)k_ovl_btn_up;
  fill_rect(out,
            w,
            h,
            cx - 1,
            (int32_t)k_pwr_y - 1,
            (int32_t)k_pwr_chg_w + 2,
            (int32_t)k_pwr_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out, w, h, cx, (int32_t)k_pwr_y, (int32_t)k_pwr_chg_w, (int32_t)k_pwr_h, face);
  draw_text(out,
            w,
            h,
            cx + (int32_t)k_btn_label_x,
            (int32_t)k_pwr_y + (int32_t)k_pwr_label_y,
            st->battery_charging ? "CHG +" : "BATT",
            (uint16_t)k_ovl_btn_label,
            1);
}

/**
 * @brief Paint the primary-core / low-power toggle on the right of the BUTTONS row.
 * @details One button captioned "<core> <power>" -- "M85 FULL" by default,
 * "M33 LP" under --primary-core m33 + low-power. The core half is read-only
 * (set by --primary-core); a click toggles the low-power half (bright when on).
 * It shares the BUTTONS row, sitting to the right of SW1/SW2; the geometry
 * matches ::board_overlay_hit_button.
 */
static void draw_core(uint16_t* out, uint16_t w, uint16_t h, int32_t x, const board_status_t* st)
{
  const int32_t  lx   = x + (int32_t)k_core_lp_off;
  const uint16_t face = st->low_power ? (uint16_t)k_ovl_btn_down : (uint16_t)k_ovl_btn_up;
  fill_rect(out,
            w,
            h,
            lx - 1,
            (int32_t)k_btn_y - 1,
            (int32_t)k_core_lp_w + 2,
            (int32_t)k_btn_h + 2,
            (uint16_t)k_ovl_btn_border);
  fill_rect(out, w, h, lx, (int32_t)k_btn_y, (int32_t)k_core_lp_w, (int32_t)k_btn_h, face);
  char cbuf[16];
  (void)snprintf(cbuf,
                 sizeof(cbuf),
                 "%s %s",
                 st->core_is_m33 ? "M33" : "M85",
                 st->low_power ? "LP" : "FULL");
  draw_text(out,
            w,
            h,
            lx + (int32_t)k_btn_label_x,
            (int32_t)k_btn_y + (int32_t)k_btn_label_y,
            cbuf,
            (uint16_t)k_ovl_btn_label,
            1);
}

/** @brief Paint the sidebar background, divider, title and all status sections. */
static void
draw_sidebar(uint16_t* out, uint16_t w, uint16_t h, uint16_t panel_w, const board_status_t* st)
{
  fill_rect(out,
            w,
            h,
            (int32_t)panel_w,
            0,
            (int32_t)k_ovl_sidebar_w,
            (int32_t)h,
            (uint16_t)k_ovl_bg);
  fill_rect(out, w, h, (int32_t)panel_w, 0, 3, (int32_t)h, (uint16_t)k_ovl_divider);
  if (st == nullptr) {
    return;
  }
  const int32_t x = (int32_t)panel_w + (int32_t)k_pad_x;
  int32_t       y = k_sidebar_top;
  /* Title bar: a filled accent band with the board name at 2x scale. */
  fill_rect(out,
            w,
            h,
            (int32_t)panel_w + 3,
            0,
            (int32_t)k_ovl_sidebar_w - 3,
            (int32_t)k_title_bar_h,
            (uint16_t)k_ovl_bg_alt);
  draw_text(out, w, h, x, y, "EK-RA8D2  EMULATOR", (uint16_t)k_ovl_accent, 2);
  y = (int32_t)k_title_bar_h + (int32_t)k_section_gap;
  draw_text(out,
            w,
            h,
            x,
            y,
            (st->app_name != nullptr) ? st->app_name : "",
            (uint16_t)k_ovl_heading,
            1);
  y += (int32_t)k_title_gap;
  /* The RUN and LEDS sections sit on the upper sidebar; the layout below them
   * (I/O, BUTTONS, CONSOLE) uses fixed Y anchors so the console always fills the
   * bottom regardless of panel height. */
  y = draw_run_stats(out, w, h, x, y, st);
  y += (int32_t)k_section_gap;
  (void)draw_leds(out, w, h, x, y, st);
  (void)draw_io_block(out, w, h, x, (int32_t)k_io_head_y, st);
  draw_power(out, w, h, x, st);
  draw_core(out, w, h, x, st);
  draw_buttons(out, w, h, x, st);
  draw_console(out, w, h, x, st);
}

void board_overlay_compose(uint16_t*             out,
                           const uint16_t*       panel,
                           uint16_t              panel_w,
                           uint16_t              panel_h,
                           const board_status_t* st)
{
  if ((out == nullptr) || (panel_w == 0U)) {
    return;
  }
  const uint16_t w = board_overlay_total_width(panel_w);
  const uint16_t h = board_overlay_total_height(panel_h);
  blit_panel(out, w, h, panel, panel_w, panel_h);
  draw_sidebar(out, w, h, panel_w, st);
}

board_overlay_btn_t board_overlay_hit_button(uint16_t x, uint16_t y, uint16_t panel_w)
{
  const int32_t cx       = (int32_t)x;
  const int32_t cy       = (int32_t)y;
  const int32_t side_lo  = (int32_t)panel_w;
  const int32_t side_hi  = (int32_t)panel_w + (int32_t)k_ovl_sidebar_w;
  const bool    in_sidex = (cx >= side_lo) && (cx < side_hi);
  /* SW1 / SW2 push-buttons. */
  if ((cy >= (int32_t)k_btn_y) && (cy < ((int32_t)k_btn_y + (int32_t)k_btn_h))) {
    const int32_t bx1 = (int32_t)panel_w + (int32_t)k_btn_x_dx;
    if ((cx >= bx1) && (cx < (bx1 + (int32_t)k_btn_w))) {
      return k_board_overlay_btn_sw1;
    }
    const int32_t bx2 = bx1 + (int32_t)k_btn_w + (int32_t)k_btn_gap;
    if ((cx >= bx2) && (cx < (bx2 + (int32_t)k_btn_w))) {
      return k_board_overlay_btn_sw2;
    }
  }
  /* POWER row: the battery slider track and the CHG toggle share one row. */
  if ((cy >= (int32_t)k_pwr_y) && (cy < ((int32_t)k_pwr_y + (int32_t)k_pwr_h))) {
    const int32_t sx = (int32_t)panel_w + (int32_t)k_pwr_x_dx;
    if ((cx >= sx) && (cx < (sx + (int32_t)k_pwr_track_w))) {
      return k_board_overlay_btn_battery;
    }
    const int32_t cgx = sx + (int32_t)k_pwr_chg_off;
    if ((cx >= cgx) && (cx < (cgx + (int32_t)k_pwr_chg_w))) {
      return k_board_overlay_btn_batt_chg;
    }
  }
  /* Core / low-power toggle: shares the BUTTONS row, right of SW1/SW2. */
  if ((cy >= (int32_t)k_btn_y) && (cy < ((int32_t)k_btn_y + (int32_t)k_btn_h))) {
    const int32_t lx = (int32_t)panel_w + (int32_t)k_btn_x_dx + (int32_t)k_core_lp_off;
    if ((cx >= lx) && (cx < (lx + (int32_t)k_core_lp_w))) {
      return k_board_overlay_btn_lowpower;
    }
  }
  /* Console region (heading + panel): a click toggles autoscroll / jumps live,
   * like clicking the Arduino Serial Monitor's pane. */
  if (in_sidex && (cy >= (int32_t)k_con_head_y)) {
    return k_board_overlay_btn_console;
  }
  return k_board_overlay_btn_none;
}

bool board_overlay_battery_pct_at(uint16_t x, uint16_t panel_w, uint8_t* out_pct)
{
  if (out_pct == nullptr) {
    return false;
  }
  const int32_t cx = (int32_t)x;
  const int32_t sx = (int32_t)panel_w + (int32_t)k_pwr_x_dx;
  if (cx <= sx) {
    *out_pct = 0U;
  } else if (cx >= (sx + (int32_t)k_pwr_track_w)) {
    *out_pct = (uint8_t)k_pwr_soc_full;
  } else {
    *out_pct = (uint8_t)(((cx - sx) * (int32_t)k_pwr_soc_full) / (int32_t)k_pwr_track_w);
  }
  return true;
}

bool board_overlay_hit_console_tab(uint16_t  x,
                                   uint16_t  y,
                                   uint16_t  panel_w,
                                   uint32_t  tab_count,
                                   uint32_t* out_idx)
{
  if (out_idx == nullptr) {
    return false;
  }
  if (tab_count == 0U) {
    return false;
  }
  uint32_t count = tab_count;
  if (count > (uint32_t)k_overlay_console_tabs_max) {
    count = (uint32_t)k_overlay_console_tabs_max;
  }
  const int32_t cx       = (int32_t)x;
  const int32_t cy       = (int32_t)y;
  const int32_t origin_x = (int32_t)panel_w + (int32_t)k_pad_x;
  const int32_t pan_w    = (int32_t)k_ovl_sidebar_w - (2 * (int32_t)k_pad_x);
  /* Point-in-cell test against the wrapping grid: each cell carries its own
   * row y, so a click resolves to the right tab on any row. The four bounds are
   * split into separate guards (no compound decision) so each edge is its own
   * branch. */
  for (uint32_t i = 0U; i < count; i++) {
    int32_t rect[4] = {0, 0, 0, 0};
    console_tab_rect(origin_x, pan_w, i, count, rect);
    if (cx < rect[0]) {
      continue;
    }
    if (cx >= (rect[0] + rect[2])) {
      continue;
    }
    if (cy < rect[1]) {
      continue;
    }
    if (cy >= (rect[1] + rect[3])) {
      continue;
    }
    *out_idx = i;
    return true;
  }
  return false;
}
