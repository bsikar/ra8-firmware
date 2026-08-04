/**
 * @file examples/ek_ra8d2/hw_validated/hil/widget_app/main.c
 * @brief On-silicon HIL: ra8_widget compositor + ra8_app framework (#145/#146).
 *
 * @details
 * End-to-end on the M85, proving the widget/app foundation works on-target:
 *
 *   1. Two **apps** (`library`, `reader`) register into an `ra8_app` registry,
 *      each running its `init` once.
 *   2. Each app is a **widget tree**: a status-bar widget (fixed height) over a
 *      content widget (flex), laid out by `ra8_widget_layout_stack` (delegating
 *      to `ra8_box`) and drawn by each widget's `render` callback through
 *      `ra8_gfx` into a 160x120 RGB565 framebuffer.
 *   3. `ra8_app_launch(library)` -> `ra8_app_render` composites the library tree;
 *      FNV-1a hash the framebuffer -> `lib_crc`.
 *   4. `ra8_app_launch(reader)` fires `library.on_leave` + `reader.on_enter`
 *      (the focus lifecycle), then `ra8_app_render` composites the reader tree
 *      -> `rdr_crc`. The two CRCs differ (different content widget).
 *   5. **Damage/partial-flush** (#145): invalidate only the status bar with the
 *      fast hint -> `ra8_widget_damage` returns just the status-bar rect + the
 *      fast refresh hint -- the minimal e-ink flush.
 *
 * The console banner on success is:
 *
 *   `widget-app-hil: apps=2 lib=<8hex> rdr=<8hex> flush=160x16 hint=fast PASS`
 *
 * Deterministic (fixed composition through deterministic layout + fills), so
 * the ra8_emulator banner is the regression net. Any failure prints a FAIL banner
 * and halts on a BKPT before PASS.
 *
 *
 * [Ring 6 / App] {World: NS}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_app.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_box.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_gfx.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"
#include "ra8_ui.h"
#include "ra8_widget.h"

/** @enum wa_consts_t @brief Console / layout knobs (no magic numbers). */
typedef enum : uint32_t {
  k_wa_uart_baud   = 115200U,     /**< Console baud.                 */
  k_wa_fb_w        = 160U,        /**< Framebuffer width, pixels.    */
  k_wa_fb_h        = 120U,        /**< Framebuffer height, pixels.   */
  k_wa_statusbar_h = 16U,         /**< Status-bar widget height.     */
  k_wa_col_bg      = 0x101018U,   /**< Framebuffer clear colour.     */
  k_wa_col_status  = 0x808088U,   /**< Status-bar fill (gray).       */
  k_wa_col_library = 0x3050C0U,   /**< Library content fill (blue).  */
  k_wa_col_reader  = 0xC05030U,   /**< Reader content fill (orange). */
  k_wa_fnv_offset  = 0x811C9DC5U, /**< FNV-1a 32-bit offset basis.   */
  k_wa_fnv_prime   = 0x01000193U, /**< FNV-1a 32-bit prime.          */
  k_wa_hex_nibbles = 8U,          /**< Hex digits in a 32-bit value. */
  k_wa_nibble_bits = 4U,          /**< Bits per hex nibble.          */
  k_wa_nibble_mask = 0x0FU,       /**< Low-nibble mask.              */
  k_wa_dec_ten     = 10U,         /**< Hex digit / decimal split.    */
} wa_consts_t;

/** @enum wa_app_id_t @brief Registered app ids. */
typedef enum : uint16_t {
  k_wa_app_library = 1U, /**< Library / home app. */
  k_wa_app_reader  = 2U, /**< EPUB reader app.    */
} wa_app_id_t;

/** @brief RGB565 framebuffer all widgets composite into. */
static uint16_t s_framebuffer[(size_t)k_wa_fb_h * (size_t)k_wa_fb_w];

/** @brief A solid-fill widget: renders one colour into its rect via ra8_gfx. */
typedef struct {
  uint32_t color; /**< 0xRRGGBB fill. */
} wa_fill_t;

/** @brief Per-app state: its widget tree + a focus-lifecycle counter. */
typedef struct {
  ra8_widget_t widgets[2];   /**< [0] status bar, [1] content. */
  wa_fill_t    status_fill;  /**< Status-bar colour.           */
  wa_fill_t    content_fill; /**< Content colour (per app).    */
  uint32_t     enters;       /**< Times on_enter fired.        */
  uint32_t     leaves;       /**< Times on_leave fired.        */
} wa_app_state_t;

static wa_app_state_t s_library;
static wa_app_state_t s_reader;

static const uint8_t k_msg_boot[]  = "widget-app-hil: boot\r\n";
static const uint8_t k_msg_fail[]  = "widget-app-hil: FAIL init\r\n";
static const uint8_t k_msg_freg[]  = "widget-app-hil: FAIL register\r\n";
static const uint8_t k_msg_fcrc[]  = "widget-app-hil: FAIL crc-equal\r\n";
static const uint8_t k_msg_flife[] = "widget-app-hil: FAIL lifecycle\r\n";
static const uint8_t k_msg_fdmg[]  = "widget-app-hil: FAIL damage\r\n";
static const uint8_t k_msg_pre[]   = "widget-app-hil: apps=2 lib=";
static const uint8_t k_msg_rdr[]   = " rdr=";
static const uint8_t k_msg_flush[] = " flush=160x16 hint=fast";
static const uint8_t k_msg_ok[]    = " PASS\r\n";

/** @brief Emit a byte run on the SCI8 console. */
static void wa_print(const uint8_t* msg, uint32_t len)
{
  (void)ra8_board_uart_console_write(msg, (size_t)len);
}

/** @brief Print the fail banner and trap (ra8_emulator halts on the BKPT). */
static void wa_panic_halt(const uint8_t* msg, uint32_t len)
{
  wa_print(msg, len);
  __asm__ volatile("bkpt #0");
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief FNV-1a hash over the composited framebuffer. */
static uint32_t wa_framebuffer_hash(void)
{
  const uint8_t* p   = (const uint8_t*)s_framebuffer;
  const size_t   n   = sizeof(s_framebuffer);
  uint32_t       hsh = (uint32_t)k_wa_fnv_offset;
  for (size_t i = 0U; i < n; i++) {
    hsh = (hsh ^ (uint32_t)p[i]) * (uint32_t)k_wa_fnv_prime;
  }
  return hsh;
}

/** @brief Print a 32-bit value as 8 upper-case hex digits. */
static void wa_print_hex(uint32_t value)
{
  uint8_t buf[k_wa_hex_nibbles];
  for (uint32_t i = 0U; i < (uint32_t)k_wa_hex_nibbles; i++) {
    const uint32_t shift = ((uint32_t)k_wa_hex_nibbles - 1U - i) * (uint32_t)k_wa_nibble_bits;
    const uint32_t nib   = (value >> shift) & (uint32_t)k_wa_nibble_mask;
    buf[i] = (uint8_t)((nib < (uint32_t)k_wa_dec_ten) ? ('0' + nib) : ('A' + (nib - k_wa_dec_ten)));
  }
  wa_print(buf, (uint32_t)k_wa_hex_nibbles);
}

/** @brief Widget render callback: fill the widget's rect with its colour. */
static void wa_fill_render(ra8_widget_t* w)
{
  const uint32_t color = ((const wa_fill_t*)w->ctx)->color;
  (void)ra8_gfx_rect(w->rect.x, w->rect.y, w->rect.w, w->rect.h, color, true);
}

/** @brief Shared vtable for the solid-fill widget. */
static const ra8_widget_vtable_t k_wa_fill_vt = {
  .measure  = nullptr,
  .render   = wa_fill_render,
  .on_input = nullptr,
};

/** @brief Build one app's widget tree: status bar (fixed) over content (flex). */
static void wa_state_init(wa_app_state_t* st, uint32_t content_color)
{
  st->status_fill.color  = (uint32_t)k_wa_col_status;
  st->content_fill.color = content_color;
  st->enters             = 0U;
  st->leaves             = 0U;

  st->widgets[0]         = (ra8_widget_t){};
  st->widgets[0].vt      = &k_wa_fill_vt;
  st->widgets[0].ctx     = &st->status_fill;
  st->widgets[0].fixed   = (int16_t)k_wa_statusbar_h;
  st->widgets[0].visible = true;

  st->widgets[1]         = (ra8_widget_t){};
  st->widgets[1].vt      = &k_wa_fill_vt;
  st->widgets[1].ctx     = &st->content_fill;
  st->widgets[1].flex    = 1U;
  st->widgets[1].visible = true;
}

/** @brief App.render: lay out + composite the app's widget tree. */
static void wa_app_render(const ra8_app_t* a)
{
  wa_app_state_t*     st = (wa_app_state_t*)a->ctx;
  ra8_box_t           scratch[4];
  const ra8_ui_rect_t frame = {.x = 0, .y = 0, .w = (int32_t)k_wa_fb_w, .h = (int32_t)k_wa_fb_h};
  (void)ra8_gfx_clear((uint32_t)k_wa_col_bg);
  (void)ra8_widget_layout_stack(st->widgets, 2U, &frame, k_ra8_widget_axis_col, 0, 0, scratch, 4U);
  (void)ra8_widget_invalidate(&st->widgets[0], k_ra8_widget_refresh_quality);
  (void)ra8_widget_invalidate(&st->widgets[1], k_ra8_widget_refresh_quality);
  (void)ra8_widget_render_dirty(st->widgets, 2U);
}

static void wa_app_enter(ra8_app_t* a)
{
  ((wa_app_state_t*)a->ctx)->enters++;
}

static void wa_app_leave(ra8_app_t* a)
{
  ((wa_app_state_t*)a->ctx)->leaves++;
}

/** @brief Shared app lifecycle vtable. */
static const ra8_app_vtable_t k_wa_app_vt = {
  .init     = nullptr,
  .on_enter = wa_app_enter,
  .tick     = nullptr,
  .render   = wa_app_render,
  .on_input = nullptr,
  .on_leave = wa_app_leave,
  .deinit   = nullptr,
};

/** @brief Bring up clocks/MSTP/time + the SCI8 console; halt on failure. */
static void wa_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  if ((ra8_cgc_init() != k_ra8_ok) || (ra8_mstp_init() != k_ra8_ok)) {
    wa_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    wa_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    wa_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
  if (ra8_board_uart_console_init((uint32_t)k_wa_uart_baud) != k_ra8_ok) {
    wa_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }
}

/** @brief Launch + composite both apps, returning each composite's hash. */
static void wa_run_two_apps(ra8_app_registry_t* reg, uint32_t* out_lib, uint32_t* out_rdr)
{
  if (ra8_app_launch(reg, (uint16_t)k_wa_app_library) != k_ra8_ok) {
    wa_panic_halt(k_msg_freg, (uint32_t)sizeof(k_msg_freg) - 1U);
  }
  (void)ra8_app_render(reg);
  *out_lib = wa_framebuffer_hash();

  if (ra8_app_launch(reg, (uint16_t)k_wa_app_reader) != k_ra8_ok) {
    wa_panic_halt(k_msg_freg, (uint32_t)sizeof(k_msg_freg) - 1U);
  }
  (void)ra8_app_render(reg);
  *out_rdr = wa_framebuffer_hash();
}

/** @brief Assert distinct composites, a fired lifecycle, and a partial flush. */
static void wa_verify_or_halt(uint32_t lib_crc, uint32_t rdr_crc)
{
  /* The two app compositions must differ (different content widget). */
  if (lib_crc == rdr_crc) {
    wa_panic_halt(k_msg_fcrc, (uint32_t)sizeof(k_msg_fcrc) - 1U);
  }
  /* The focus lifecycle must have fired exactly once each. */
  if ((s_library.leaves != 1U) || (s_reader.enters != 1U) || (s_library.enters != 1U)) {
    wa_panic_halt(k_msg_flife, (uint32_t)sizeof(k_msg_flife) - 1U);
  }
  /* Partial-flush: invalidate ONLY the status bar -> damage is just its rect. */
  (void)ra8_widget_invalidate(&s_reader.widgets[0], k_ra8_widget_refresh_fast);
  ra8_ui_rect_t        dmg    = {};
  ra8_widget_refresh_t hint   = k_ra8_widget_refresh_none;
  uint16_t             ndirty = 0U;
  (void)ra8_widget_damage(s_reader.widgets, 2U, &dmg, &hint, &ndirty);
  if ((ndirty != 1U) || (dmg.h != (int32_t)k_wa_statusbar_h) || (dmg.y != 0) ||
      (dmg.w != (int32_t)k_wa_fb_w) || (hint != k_ra8_widget_refresh_fast)) {
    wa_panic_halt(k_msg_fdmg, (uint32_t)sizeof(k_msg_fdmg) - 1U);
  }
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: register two widget-tree apps, launch + composite each.
 *
 * @return Never returns.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post The apps/CRC/flush banner is emitted; CPU loops in WFI.
 * @since 0.1.0
 */
int32_t main(void)
{
  wa_setup_or_halt();
  ra8_isr_globals_enable();
  wa_print(k_msg_boot, (uint32_t)sizeof(k_msg_boot) - 1U);

  if (ra8_gfx_init(s_framebuffer,
                   (uint16_t)k_wa_fb_w,
                   (uint16_t)k_wa_fb_h,
                   k_ra8_gfx_format_rgb565) != k_ra8_ok) {
    wa_panic_halt(k_msg_fail, (uint32_t)sizeof(k_msg_fail) - 1U);
  }

  /* Two apps, each a widget tree. */
  wa_state_init(&s_library, (uint32_t)k_wa_col_library);
  wa_state_init(&s_reader, (uint32_t)k_wa_col_reader);
  ra8_app_t library = {.vt        = &k_wa_app_vt,
                       .ctx       = &s_library,
                       .id        = (uint16_t)k_wa_app_library,
                       .name      = "Library",
                       .removable = false};
  ra8_app_t reader  = {.vt        = &k_wa_app_vt,
                       .ctx       = &s_reader,
                       .id        = (uint16_t)k_wa_app_reader,
                       .name      = "Reader",
                       .removable = false};

  ra8_app_t*         slots[2];
  ra8_app_registry_t reg = {};
  if ((ra8_app_registry_init(&reg, slots, 2U) != k_ra8_ok) ||
      (ra8_app_register(&reg, &library) != k_ra8_ok) ||
      (ra8_app_register(&reg, &reader) != k_ra8_ok)) {
    wa_panic_halt(k_msg_freg, (uint32_t)sizeof(k_msg_freg) - 1U);
  }

  uint32_t lib_crc = 0U;
  uint32_t rdr_crc = 0U;
  wa_run_two_apps(&reg, &lib_crc, &rdr_crc);
  wa_verify_or_halt(lib_crc, rdr_crc);

  wa_print(k_msg_pre, (uint32_t)sizeof(k_msg_pre) - 1U);
  wa_print_hex(lib_crc);
  wa_print(k_msg_rdr, (uint32_t)sizeof(k_msg_rdr) - 1U);
  wa_print_hex(rdr_crc);
  wa_print(k_msg_flush, (uint32_t)sizeof(k_msg_flush) - 1U);
  wa_print(k_msg_ok, (uint32_t)sizeof(k_msg_ok) - 1U);

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
