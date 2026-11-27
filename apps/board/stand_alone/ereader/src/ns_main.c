/**
 * @file apps/board/stand_alone/ereader/src/ns_main.c
 * @brief Non-Secure main entry point: launches ThreadX, touch, SD/VFS, and the reader subsystem.
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * This file is executed after the Secure world bootloader configures the SAU
 * and jumps to ns_reset_handler. It operates entirely in Non-Secure state,
 * leveraging ThreadX for multitasking and calling Secure services via
 * NSC veneers.
 *
 * A ThreadX watchdog supervisor guards the workers: at start-up it arms the
 * Secure-owned WDT through the ``ra8_nsc_wdt_start`` veneer, then refreshes it
 * (through ``ra8_nsc_wdt_refresh``) only while every worker thread keeps
 * checking in within its deadline.
 *
 * The Non-Secure application brings up:
 *   - The EK-RA8D2 GLCDC panel + SDRAM framebuffer via ``ra8_display_pal``.
 *   - Backlight policy with 30s timeout and activity wake.
 *   - Touch polling via ``ra8_nsc_board_touch_open`` and ``ra8_nsc_touch_read``.
 *   - Storage through ``ra8_board_sdhi_pins_init``, ``ra8_sdcard_init``,
 *     ``ra8_io_blockdev_sdhi_init``, and ``epub_open_streamed_fs``.
 *   - Reader subsystem with ``reflow`` layout, touch edge page turns,
 *     and tap-to-zoom raster figures via ``zoom_popup``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "epub.h"
#include "epub_fs.h"
#include "er_lowpower.h"
#include "er_ns_nsc_guard.h"
#include "er_pageturn.h"
#include "er_panel.h"
#include "figure_fixture.h"
#include "literata_latin1.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_display_pal_backlight.h"
#include "ra8_err.h"
#include "ra8_fs.h"
#include "ra8_gfx.h"
#include "ra8_io.h"
#include "ra8_io_blockdev.h"
#include "ra8_io_blockdev_sdhi.h"
#include "ra8_io_vfs.h"
#include "ra8_nsc.h"
#include "ra8_nsc_io.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_sdcard.h"
#include "ra8_wdt_supervisor.h"
#include "reflow.h"
#include "reflow_image.h"
#include "tx_api.h"
#include "zoom.h"
#include "zoom_mem.h"
#include "zoom_popup.h"

/* Linker symbols for BSS and Stack */
extern uint32_t g_ra8_ls_ns_bss_start;
extern uint32_t g_ra8_ls_ns_bss_end;
extern uint32_t g_ra8_ls_ns_stack_top;
extern uint32_t g_ra8_ls_ns_run_start;

#ifdef RA8_EREADER_NS_XIP
extern uint32_t g_ra8_ls_ns_data_load_start;
extern uint32_t g_ra8_ls_ns_data_start;
extern uint32_t g_ra8_ls_ns_data_end;
#endif

/** @brief Address of NS VTOR register. */
typedef enum : uintptr_t {
  k_ns_scb_vtor_addr = 0xE000ED08U, /**< NS SCB VTOR address. */
} ns_scb_addr_t;

/**
 * @enum ns_thread_stack_t
 * @brief Stack sizes for Non-Secure ThreadX threads.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_thread_stack_size  = 16384U, /**< Stack size for UI thread.     */
  k_ns_sys_thread_stack_size = 16384U, /**< Stack size for System thread. */
} ns_thread_stack_t;

static TX_THREAD s_ui_thread;
static TX_THREAD s_sys_thread;

static uint8_t s_ui_thread_stack[k_ns_ui_thread_stack_size];
static uint8_t s_sys_thread_stack[k_ns_sys_thread_stack_size];

/**
 * @enum ns_log_cadence_t
 * @brief Heartbeat-log cadence per thread (iterations between log lines).
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_heartbeat_frames = 60U, /**< UI heartbeat every ~60 frames (~1 s).    */
  k_ns_sys_heartbeat_iters = 10U, /**< System heartbeat every ~10 loops (~1 s). */
} ns_log_cadence_t;

/**
 * @enum ns_thread_sleep_t
 * @brief Per-thread loop sleep period, in ThreadX ticks.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_frame_ticks = 16U,  /**< UI loop sleep (~16 ms, ~60 FPS).     */
  k_ns_sys_poll_ticks = 100U, /**< System loop sleep (~100 ms cadence). */
} ns_thread_sleep_t;

/**
 * @enum ns_thread_prio_t
 * @brief ThreadX priority + preemption threshold for each Non-Secure thread.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_priority  = 10U, /**< UI thread priority + preemption threshold.     */
  k_ns_sys_priority = 15U, /**< System thread priority + preemption threshold. */
} ns_thread_prio_t;

/**
 * @enum ns_wdt_param_t
 * @brief Watchdog-supervisor sizing and timing parameters.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_wdt_sup_stack_size  = 1024U, /**< Supervisor thread stack size (bytes).   */
  k_ns_wdt_ui_deadline_ms  = 500U,  /**< UI worker check-in deadline (ms).       */
  k_ns_wdt_sys_deadline_ms = 1000U, /**< System worker check-in deadline (ms).   */
  k_ns_wdt_refresh_ms      = 50U,   /**< Supervisor refresh / poll cadence (ms). */
} ns_wdt_param_t;

/**
 * @enum ns_wdt_prio_t
 * @brief ThreadX priority for the watchdog supervisor thread.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_wdt_sup_priority = 14U, /**< Supervisor thread priority. */
} ns_wdt_prio_t;

/** @brief Supervisor thread stack (caller-owned, 8-byte aligned for ThreadX). */
[[gnu::aligned(8)]] static uint8_t s_sup_stack[k_ns_wdt_sup_stack_size];

/** @brief Supervisor check-in handle for the UI thread. */
static uint8_t s_ui_wdt_handle = (uint8_t)k_ra8_wdt_sup_handle_invalid;
/** @brief Supervisor check-in handle for the system thread. */
static uint8_t s_sys_wdt_handle = (uint8_t)k_ra8_wdt_sup_handle_invalid;

/**
 * @enum er_reader_layout_t
 * @brief Frame geometry and styling metrics for the reader view.
 */
typedef enum : uint32_t {
  k_er_statusbar_h     = 64U,                /**< Status bar height (pixels).         */
  k_er_footer_h        = 56U,                /**< Footer band height (pixels).        */
  k_er_margin_x        = 64U,                /**< Horizontal text margin (pixels).    */
  k_er_body_gap        = 24U,                /**< Vertical band-to-body gap (pixels). */
  k_er_reflow_px       = 22U,                /**< Body font size in pixels.           */
  k_er_zoom_strip_rows = 16U,                /**< Composited rows per zoom strip.     */
  k_er_zoom_scale_max  = 4U,                 /**< Maximum figure zoom scale.          */
  k_er_fig_gray8_cap   = 512U * 1024U,       /**< Decoded figure gray8 capacity.      */
  k_er_img_arena       = 2U * 1024U * 1024U, /**< SDRAM image decode arena.           */
  k_er_reflow_ink      = 0xFF101010U,        /**< Reflow text color (near-black).     */
  k_er_reflow_link     = 0xFF2A52BEU,        /**< Reflow link color (cerulean).       */
  k_er_color_statusbar = 0x1A2530U,          /**< Status bar background color.        */
  k_er_color_paper     = 0xFFFFFFU,          /**< Paper background color.             */
  k_er_color_rule      = 0xAAAAAAU,          /**< Footer hairline rule color.         */
} er_reader_layout_t;

/* Public telemetry globals for HIL and emulator inspection */
volatile uint32_t  g_ns_ui_frames             = 0U;
volatile uint32_t  g_ns_sys_ticks             = 0U;
volatile uint32_t  g_ns_backlight_state       = (uint32_t)k_display_backlight_state_off;
volatile uint32_t  g_ns_backlight_pct         = 0U;
volatile uint32_t  g_ns_backlight_transitions = 0U;
volatile ra8_err_t g_ns_backlight_init_err    = k_ra8_ok;
volatile uint32_t  g_ns_panel_ready           = 0U;
volatile ra8_err_t g_ns_panel_init_err        = k_ra8_ok;
volatile uint32_t  g_ns_refresh_mode          = (uint32_t)k_er_refresh_fast_clean;
volatile uint32_t  g_ns_refresh_hint          = (uint32_t)k_display_refresh_init;
volatile uint32_t  g_ns_frames_flushed        = 0U;
volatile uint32_t  g_ns_fb_crc                = 0U;

volatile uint32_t g_ns_touch_ready      = 0U;
volatile uint32_t g_ns_touch_events     = 0U;
volatile uint32_t g_ns_sd_ready         = 0U;
volatile uint32_t g_ns_book_ready       = 0U;
volatile uint32_t g_ns_current_chapter  = 0U;
volatile uint32_t g_ns_current_page     = 0U;
volatile uint32_t g_ns_total_pages      = 1U;
volatile uint32_t g_ns_total_chapters   = 2U;
volatile uint32_t g_ns_page_turns       = 0U;
volatile uint32_t g_ns_fig_active       = 0U;
volatile uint32_t g_ns_fig_scale        = 0U;
volatile uint32_t g_ns_manga_active     = 0U;
volatile uint32_t g_ns_translate_active = 0U;
volatile uint32_t g_ns_idle_handoff_requests = 0U;
volatile uint32_t g_ns_lpm_standby_wakes     = 0U;

/** @brief 1024x600 RGB565 panel framebuffer placed in NOLOAD external SDRAM. */
RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);

/* External SDRAM buffers preserving internal SRAM */
[[gnu::section(".sdram_data")]] static reflow_t s_reflow_engine;
[[gnu::section(".sdram_data")]] static uint8_t  s_fig_gray8[k_er_fig_gray8_cap];
[[gnu::section(".sdram_data")]] static uint8_t  s_zoom_row[k_panel_width_px];
[[gnu::section(".sdram_data")]] static uint8_t
  s_zoom_strip[(size_t)k_panel_width_px * (size_t)k_er_zoom_strip_rows];
[[gnu::section(".sdram_data")]] static uint8_t
  s_zoom_packed[((size_t)k_panel_width_px * (size_t)k_er_zoom_strip_rows) / 2U];
[[gnu::section(".sdram_data")]] static uint8_t s_img_arena_buf[k_er_img_arena];
[[gnu::section(".sdram_data")]] static uint8_t s_img_res_buf[128U * 1024U];
[[gnu::section(".sdram_data")]] static uint8_t s_chapter_xhtml[64U * 1024U];

static er_panel_state_t           s_ns_panel                   = {};
static display_backlight_t        s_ns_backlight               = {};
static display_backlight_policy_t s_ns_backlight_policy        = {};
static ra8_nsc_backlight_caps_t   s_ns_backlight_caps          = {};
static bool                       s_ns_backlight_ready         = false;
static volatile bool              s_backlight_activity_pending = false;

static ra8_io_blockdev_t                                    s_sd_bd       = {};
static ra8_fs_backend_t                                     s_sd_be       = {};
static ra8_fs_mount_t*                                      s_sd_mount    = nullptr;
[[gnu::section(".sdram_data")]] static epub_stream_fs_ctx_t s_epub_io     = {};
[[gnu::section(".sdram_data")]] static epub_book_t          s_epub_book   = {};
static bool                                                 s_book_loaded = false;

static zoom_mem_src_t  s_fig_mem          = {};
static zoom_popup_t    s_fig_popup        = {};
static ra8_img_arena_t s_reflow_img_arena = {};
static bool            s_reader_ready     = false;

typedef struct {
  const char* xhtml;
  uint32_t    len;
  const char* href;
  uint32_t    href_len;
} er_spine_chapter_t;

static const char k_er_chapter1_xhtml[] =
  "<html><body><h1>Chapter I: Down the Rabbit-Hole</h1>"
  "<p>Alice was beginning to get very tired of sitting by her sister on the bank, "
  "and of having nothing to do: once or twice she had peeped into the book her sister was "
  "reading, but it had no pictures or conversations in it.</p>"
  "<img src=\"figure.png\"/>"
  "<p>So she was considering in her own mind whether the pleasure of making a daisy-chain "
  "would be worth the trouble of getting up and picking the daisies, when suddenly a White "
  "Rabbit with pink eyes ran close by her.</p></body></html>";

static const char k_er_chapter2_xhtml[] =
  "<html><body><h1>Chapter II: The Pool of Tears</h1>"
  "<p>'Curiouser and curiouser!' cried Alice (she was so much surprised, that for the moment "
  "she quite forgot how to speak good English); 'now I'm opening out like the largest "
  "telescope that ever was! Good-bye, feet!'</p>"
  "<p>And she went on rambling: 'Oh, my poor little feet, I wonder who will put on your shoes "
  "and stockings for you now, dears?'</p></body></html>";

static const er_spine_chapter_t k_er_builtin_spine[] = {
  {k_er_chapter1_xhtml, (uint32_t)(sizeof(k_er_chapter1_xhtml) - 1U), "ch1.xhtml", 9U},
  {k_er_chapter2_xhtml, (uint32_t)(sizeof(k_er_chapter2_xhtml) - 1U), "ch2.xhtml", 9U},
};

/** @brief Convert ThreadX kernel ticks into monotonic milliseconds. */
RA8_INTERNAL static uint32_t internal_ns_now_ms(void)
{
  return (uint32_t)(((uint64_t)tx_time_get() * 1000ULL) / (uint64_t)TX_TIMER_TICKS_PER_SECOND);
}

/** @brief Guarded NSC log helper preventing SysTick_NS preemption in Secure mode. */
RA8_INTERNAL static ra8_err_t internal_ns_log_emit(const char* tag, const char* msg)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_log_emit(tag, msg));
  return err;
}

/** @brief Guarded WDT refresh hook registered with ThreadX WDT supervisor. */
RA8_INTERNAL static void internal_ns_wdt_refresh(void)
{
  ER_NS_NSC_VOID(ra8_nsc_wdt_refresh());
}

/** @brief Backlight enable callback bridged across TrustZone to Secure board GPIO. */
RA8_INTERNAL static ra8_err_t internal_ns_backlight_set_cb(void* ctx, uint8_t on)
{
  (void)ctx;
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_backlight_set(on != 0U));
  return err;
}

/** @brief Signal user or display activity to wake the NS backlight idle timer. */
void ra8_ereader_backlight_notify_activity(void);
void ra8_ereader_backlight_notify_activity(void)
{
  s_backlight_activity_pending = true;
}

/** @brief Compute deterministic 32-bit FNV-1a digest over the SDRAM panel framebuffer. */
RA8_INTERNAL static uint32_t internal_ns_fb_checksum(void)
{
  uint32_t       h     = 2166136261U;
  const uint8_t* bytes = (const uint8_t*)s_framebuffer;
  for (size_t i = 0U; i < sizeof(s_framebuffer); ++i) {
    h = (h ^ (uint32_t)bytes[i]) * 16777619U;
  }
  return h;
}

/** @brief Sync exported display/refresh inspection variables from active panel state. */
RA8_INTERNAL static void internal_ns_panel_publish(void)
{
  g_ns_refresh_mode   = (uint32_t)s_ns_panel.mode;
  g_ns_refresh_hint   = (uint32_t)s_ns_panel.last_hint;
  g_ns_frames_flushed = s_ns_panel.frame_count;
  g_ns_fb_crc         = internal_ns_fb_checksum();
}

/** @brief Initialize SDRAM + EK-RA8D2 GLCDC panel via display PAL and render first frame. */
RA8_INTERNAL static void internal_ns_panel_setup(void)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_sdramc_init());
  if (err != k_ra8_ok) {
    g_ns_panel_init_err = err;
    (void)internal_ns_log_emit("UI", "Panel SDRAM init failed");
    return;
  }

  const display_fb_cfg_t fb_cfg = {
    .pixels    = s_framebuffer,
    .bytes     = (uint32_t)sizeof(s_framebuffer),
    .width_px  = (uint16_t)k_panel_width_px,
    .height_px = (uint16_t)k_panel_height_px,
    .pixfmt    = k_display_pixfmt_rgb565,
  };
  err = er_panel_init_glcdc(&s_ns_panel,
                            &fb_cfg,
                            &s_ra8_panel_ek_ra8d2_timing,
                            k_er_refresh_fast_clean,
                            (uint16_t)k_display_policy_clean_every_default);
  if (err != k_ra8_ok) {
    g_ns_panel_init_err = err;
    (void)internal_ns_log_emit("UI", "Panel display PAL init failed");
    return;
  }

  g_ns_panel_init_err = k_ra8_ok;
  g_ns_panel_ready    = 1U;
  internal_ns_panel_publish();
  (void)internal_ns_log_emit("UI", "Panel initialized via display PAL");
}

/** @brief Bring up the NS backlight controller, 30s idle policy, and wake switches. */
RA8_INTERNAL static void internal_ns_backlight_setup(uint32_t now_ms)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_backlight_get_caps(&s_ns_backlight_caps));
  if (err != k_ra8_ok) {
    g_ns_backlight_init_err = err;
    return;
  }
  if (g_ns_panel_ready == 0U) {
    ER_NS_NSC_CALL(err, ra8_nsc_backlight_init(false));
    if (err != k_ra8_ok) {
      g_ns_backlight_init_err = err;
      return;
    }
  }

  const display_backlight_cfg_t cfg = {
    .mode       = k_display_backlight_mode_binary,
    .set_enable = internal_ns_backlight_set_cb,
    .ctx        = nullptr,
  };
  err = display_backlight_init(&s_ns_backlight, &cfg);
  if (err != k_ra8_ok) {
    g_ns_backlight_init_err = err;
    return;
  }

  const uint8_t  supports_dim   = s_ns_backlight_caps.supports_pwm ? 1U : 0U;
#ifdef RA8_EREADER_LP_EMU_SOAK
  const uint32_t off_timeout_ms = 80U;
#else
  const uint32_t off_timeout_ms = (uint32_t)k_display_backlight_default_off_ms;
#endif
  const display_backlight_policy_cfg_t pol_cfg      = {
    .dim_after_ms = (supports_dim != 0U) ? (uint32_t)k_display_backlight_default_dim_ms : 0U,
    .off_after_ms = off_timeout_ms,
    .active_pct   = (uint8_t)k_display_backlight_default_active_pct,
    .dimmed_pct   = (uint8_t)k_display_backlight_default_dimmed_pct,
    .supports_dim = supports_dim,
  };
  err = display_backlight_policy_init(&s_ns_backlight_policy, &pol_cfg, now_ms);
  if (err != k_ra8_ok) {
    g_ns_backlight_init_err = err;
    return;
  }

  err = display_backlight_set_brightness(&s_ns_backlight,
                                         (uint8_t)k_display_backlight_default_active_pct);
  if (err != k_ra8_ok) {
    g_ns_backlight_init_err = err;
    return;
  }

  ER_NS_NSC_VOID((void)ra8_nsc_sw_init(0U));
  ER_NS_NSC_VOID((void)ra8_nsc_sw_init(1U));

  g_ns_backlight_init_err = k_ra8_ok;
  g_ns_backlight_state    = (uint32_t)s_ns_backlight_policy.state;
  g_ns_backlight_pct      = (uint32_t)s_ns_backlight.brightness_pct;
  s_ns_backlight_ready    = true;
}

/** @brief Pump user button edges and advance the 30s backlight idle policy. */
RA8_INTERNAL static void internal_ns_backlight_tick(bool* prev_sw1,
                                                    bool* prev_sw2,
                                                    bool* out_sw1_edge,
                                                    bool* out_sw2_edge,
                                                    bool* out_sw1_down,
                                                    bool* out_sw2_down)
{
  *out_sw1_edge = false;
  *out_sw2_edge = false;
  *out_sw1_down = false;
  *out_sw2_down = false;
  if (!s_ns_backlight_ready) {
    return;
  }

  bool sw1_now = false;
  bool sw2_now = false;
  ER_NS_NSC_VOID((void)ra8_nsc_sw_read(0U, &sw1_now));
  ER_NS_NSC_VOID((void)ra8_nsc_sw_read(1U, &sw2_now));

  const bool sw1_edge = sw1_now && !(*prev_sw1);
  const bool sw2_edge = sw2_now && !(*prev_sw2);
  const bool sw_edge  = sw1_edge || sw2_edge;
  *prev_sw1           = sw1_now;
  *prev_sw2           = sw2_now;
  *out_sw1_edge       = sw1_edge;
  *out_sw2_edge       = sw2_edge;
  *out_sw1_down       = sw1_now;
  *out_sw2_down       = sw2_now;

  const uint32_t                      now_ms = internal_ns_now_ms();
  display_backlight_policy_decision_t dec    = {};
  ra8_err_t                           err    = k_ra8_ok;
  if (sw_edge || s_backlight_activity_pending) {
    s_backlight_activity_pending = false;
    err = display_backlight_policy_activity(&s_ns_backlight_policy, now_ms, &dec);
  } else {
    err = display_backlight_policy_step(&s_ns_backlight_policy, now_ms, &dec);
  }

  if ((err == k_ra8_ok) && (dec.changed != 0U)) {
    if (display_backlight_apply_decision(&s_ns_backlight, &dec) == k_ra8_ok) {
      g_ns_backlight_transitions++;
      g_ns_backlight_state = (uint32_t)dec.state;
      g_ns_backlight_pct   = (uint32_t)dec.target_pct;
    }
  }
}

/** @brief Image loader callback for reflow: provides PNG data from EPUB or fixture. */
RA8_INTERNAL static ra8_err_t internal_ns_image_loader(void*           ctx,
                                                       const char*     href,
                                                       uint32_t        href_len,
                                                       const uint8_t** out_bytes,
                                                       size_t*         out_len)
{
  (void)ctx;
  (void)href_len;
  if (s_book_loaded && (href != nullptr)) {
    size_t got = 0U;
    if ((epub_get_resource(&s_epub_book, href, s_img_res_buf, sizeof(s_img_res_buf), &got) ==
         k_ra8_ok) &&
        (got > 0U)) {
      *out_bytes = s_img_res_buf;
      *out_len   = got;
      return k_ra8_ok;
    }
  }
  *out_bytes = k_er_figure_png;
  *out_len   = (size_t)k_er_figure_png_len;
  return k_ra8_ok;
}

/** @brief Build the SDRAM scratch descriptor for zoom_popup. */
RA8_INTERNAL static zoom_scratch_t internal_ns_fig_scratch(void)
{
  return (zoom_scratch_t){
    .row        = s_zoom_row,
    .row_cap    = (uint32_t)sizeof(s_zoom_row),
    .strip      = s_zoom_strip,
    .strip_cap  = (uint32_t)sizeof(s_zoom_strip),
    .packed     = s_zoom_packed,
    .packed_cap = (uint32_t)sizeof(s_zoom_packed),
  };
}

/** @brief Render figure zoom popup and flush its bounding rect. */
RA8_INTERNAL static void internal_ns_figure_present(void)
{
  if (zoom_popup_render(&s_fig_popup) != k_ra8_ok) {
    return;
  }
  zoom_present_t plan = {};
  if ((zoom_popup_present(&s_fig_popup, &plan) != k_ra8_ok) || !plan.present) {
    return;
  }
  const display_refresh_hint_t hint =
    (plan.refresh == k_zoom_refresh_fast) ? k_display_refresh_fast : k_display_refresh_quality;
  const display_rect_t r = {
    .x = (uint16_t)((plan.rect.x >= 0) ? plan.rect.x : 0),
    .y = (uint16_t)((plan.rect.y >= 0) ? plan.rect.y : 0),
    .w = (uint16_t)((plan.rect.w >= 0) ? plan.rect.w : 0),
    .h = (uint16_t)((plan.rect.h >= 0) ? plan.rect.h : 0),
  };
  (void)display_flush(s_ns_panel.display, r, hint);
  s_ns_panel.last_hint = hint;
  s_ns_panel.last_rect = r;
  s_ns_panel.frame_count++;
  internal_ns_panel_publish();
}

/** @brief Decode raster figure and open zoom popup overlay. */
RA8_INTERNAL static bool internal_ns_figure_open(uint32_t img_idx, int32_t tx, int32_t ty)
{
  const reflow_image_box_t* box   = &s_reflow_engine.image_boxes[img_idx];
  const uint8_t*            bytes = nullptr;
  size_t                    len   = 0U;
  const char*               href  = (const char*)&s_reflow_engine.text_pool[box->src_off];
  if (internal_ns_image_loader(nullptr, href, box->src_len, &bytes, &len) != k_ra8_ok) {
    return false;
  }
  ra8_img_arena_t arena = {
    .base   = s_img_arena_buf,
    .cap    = (size_t)k_er_img_arena,
    .offset = 0U,
    .live   = 0U,
  };
  uint32_t img_w = 0U;
  uint32_t img_h = 0U;
  if (ra8_img_decode_gray8(&arena, bytes, len, s_fig_gray8, sizeof(s_fig_gray8), &img_w, &img_h) !=
      k_ra8_ok) {
    return false;
  }
  zoom_source_t zs = {};
  if ((zoom_mem_src_init(&s_fig_mem, s_fig_gray8, img_w, img_h, 0U) != k_ra8_ok) ||
      (zoom_mem_src_bind(&s_fig_mem, &zs) != k_ra8_ok)) {
    return false;
  }
  const int32_t focus_x = (box->w > 0)
                            ? (int32_t)(((int64_t)(tx - box->x) * (int64_t)img_w) / (int64_t)box->w)
                            : (int32_t)(img_w / 2U);
  const int32_t focus_y = (box->h > 0)
                            ? (int32_t)(((int64_t)(ty - box->y) * (int64_t)img_h) / (int64_t)box->h)
                            : (int32_t)(img_h / 2U);
  const zoom_popup_cfg_t cfg = {
    .src       = zs,
    .scratch   = internal_ns_fig_scratch(),
    .fb_w      = (uint16_t)k_panel_width_px,
    .fb_h      = (uint16_t)k_panel_height_px,
    .scale     = (uint8_t)k_zoom_scale_min,
    .scale_max = (uint8_t)k_er_zoom_scale_max,
    .policy    = k_zoom_policy_responsive,
    .focus_x   = focus_x,
    .focus_y   = focus_y,
  };
  if (zoom_popup_open(&s_fig_popup, &cfg) != k_ra8_ok) {
    return false;
  }
  g_ns_fig_active = 1U;
  g_ns_fig_scale  = (uint32_t)s_fig_popup.view.scale;
  internal_ns_figure_present();
  (void)internal_ns_log_emit("UI", "Figure zoom opened");
  return true;
}

/** @brief Format a positive integer into a buffer and return characters appended. */
RA8_INTERNAL static size_t internal_ns_append_uint(char* buf, size_t cap, size_t pos, uint32_t val)
{
  char     tmp[16];
  uint32_t v = val;
  size_t   n = 0U;
  do {
    tmp[n++] = (char)('0' + (v % 10U));
    v /= 10U;
  } while ((v > 0U) && (n < sizeof(tmp)));

  while ((n > 0U) && (pos < (cap - 1U))) {
    buf[pos++] = tmp[--n];
  }
  buf[pos] = '\0';
  return pos;
}

/** @brief Format string literal into buffer. */
RA8_INTERNAL static size_t
internal_ns_append_str(char* buf, size_t cap, size_t pos, const char* str)
{
  if (str == nullptr) {
    return pos;
  }
  while ((*str != '\0') && (pos < (cap - 1U))) {
    buf[pos++] = *str++;
  }
  buf[pos] = '\0';
  return pos;
}

/** @brief Render the current reader page (status bar, reflow body, footer) and flush. */
RA8_INTERNAL static void internal_ns_render_page(void)
{
  if (!s_ns_panel.initialized || !s_reader_ready) {
    return;
  }
  const int32_t w        = (int32_t)k_panel_width_px;
  const int32_t h        = (int32_t)k_panel_height_px;
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;

  (void)ra8_gfx_clear((uint32_t)k_er_color_paper);
  (void)ra8_gfx_rect(0, 0, w, (int32_t)k_er_statusbar_h, (uint32_t)k_er_color_statusbar, true);

  const char* title = (s_book_loaded && (s_epub_book.title[0] != '\0'))
                        ? s_epub_book.title
                        : "Alice's Adventures in Wonderland";
  (void)ra8_gfx_text_out((int32_t)k_er_margin_x,
                         24,
                         title,
                         &ra8_gfx_font_8x16,
                         (uint32_t)k_er_color_paper,
                         (uint32_t)k_er_color_statusbar);

  if (g_ns_current_page >= g_ns_total_pages) {
    g_ns_current_page = (g_ns_total_pages > 0U) ? (g_ns_total_pages - 1U) : 0U;
  }
  (void)
    reflow_render_page_at(&s_reflow_engine, g_ns_current_page, (int32_t)k_er_margin_x, body_top);

  const int32_t footer_rule_y = h - (int32_t)k_er_footer_h;
  (void)ra8_gfx_line((int32_t)k_er_margin_x,
                     footer_rule_y,
                     w - (int32_t)k_er_margin_x,
                     footer_rule_y,
                     (uint32_t)k_er_color_rule);

  char   footer_buf[96] = {};
  size_t pos            = 0U;
  pos                   = internal_ns_append_str(footer_buf, sizeof(footer_buf), pos, "Page ");
  pos = internal_ns_append_uint(footer_buf, sizeof(footer_buf), pos, g_ns_current_page + 1U);
  pos = internal_ns_append_str(footer_buf, sizeof(footer_buf), pos, " of ");
  pos = internal_ns_append_uint(footer_buf, sizeof(footer_buf), pos, g_ns_total_pages);
  pos = internal_ns_append_str(footer_buf, sizeof(footer_buf), pos, "  |  Chapter ");
  pos = internal_ns_append_uint(footer_buf, sizeof(footer_buf), pos, g_ns_current_chapter + 1U);
  pos = internal_ns_append_str(footer_buf, sizeof(footer_buf), pos, " of ");
  pos = internal_ns_append_uint(footer_buf, sizeof(footer_buf), pos, g_ns_total_chapters);

  (void)ra8_gfx_text_out((int32_t)k_er_margin_x,
                         footer_rule_y + 16,
                         footer_buf,
                         &ra8_gfx_font_8x16,
                         0x555555U,
                         (uint32_t)k_er_color_paper);

  display_policy_decision_t dec = {};
  (void)display_policy_decide(&s_ns_panel.policy, k_display_event_turn, &dec);
  const display_rect_t full_rect = display_full_rect(s_ns_panel.display);
  (void)display_flush(s_ns_panel.display, full_rect, dec.hint);
  s_ns_panel.last_hint = dec.hint;
  s_ns_panel.last_rect = full_rect;
  s_ns_panel.frame_count++;
  internal_ns_panel_publish();
}

/** @brief Load and layout a chapter (either from open EPUB or built-in spine). */
RA8_INTERNAL static void internal_ns_load_chapter(uint32_t ch_idx)
{
  if (!s_reader_ready) {
    return;
  }
  uint32_t pages = 1U;
  if (s_book_loaded) {
    size_t got = 0U;
    if ((epub_load_chapter(&s_epub_book,
                           (uint16_t)ch_idx,
                           s_chapter_xhtml,
                           sizeof(s_chapter_xhtml),
                           &got) == k_ra8_ok) &&
        (got > 0U)) {
      if (reflow_layout_chapter(&s_reflow_engine, s_chapter_xhtml, got, &pages) == k_ra8_ok) {
        g_ns_total_pages     = (pages > 0U) ? pages : 1U;
        g_ns_current_chapter = ch_idx;
        return;
      }
    }
  }

  const uint32_t idx = (ch_idx < 2U) ? ch_idx : 0U;
  if (reflow_layout_chapter(&s_reflow_engine,
                            (const uint8_t*)k_er_builtin_spine[idx].xhtml,
                            k_er_builtin_spine[idx].len,
                            &pages) == k_ra8_ok) {
    g_ns_total_pages     = (pages > 0U) ? pages : 1U;
    g_ns_current_chapter = idx;
  }
}

/** @brief Advance or retreat reading location by one page turn. */
RA8_INTERNAL static void internal_ns_handle_page_turn(er_dir_t dir)
{
  uint32_t next_ch = g_ns_current_chapter;
  uint32_t next_p  = g_ns_current_page;
  bool     crossed = false;
  if (er_pageturn_step(g_ns_current_chapter,
                       g_ns_current_page,
                       g_ns_total_pages,
                       g_ns_total_chapters,
                       dir,
                       &next_ch,
                       &next_p,
                       &crossed)) {
    g_ns_page_turns++;
    if (crossed) {
      g_ns_current_chapter = next_ch;
      internal_ns_load_chapter(next_ch);
      g_ns_current_page = (next_p == k_er_page_last) ? (g_ns_total_pages - 1U) : 0U;
    } else {
      g_ns_current_page = next_p;
    }
    internal_ns_render_page();
    (void)internal_ns_log_emit("UI", "Page turn executed");
  }
}

/** @brief Hit test tap against images in the active reflow layout. */
RA8_INTERNAL static bool internal_ns_try_figure_tap(int32_t x, int32_t y)
{
  const int32_t body_top = (int32_t)k_er_statusbar_h + (int32_t)k_er_body_gap;
  const int32_t tx       = x - (int32_t)k_er_margin_x;
  const int32_t ty       = y - body_top;
  uint32_t      img_idx  = 0U;
  if (reflow_hit_test_image(&s_reflow_engine, g_ns_current_page, tx, ty, &img_idx) == k_ra8_ok) {
    return internal_ns_figure_open(img_idx, tx, ty);
  }
  return false;
}

/** @brief Initialize reflow engine and layout initial chapter. */
RA8_INTERNAL static void internal_ns_reader_setup(void)
{
  const int32_t body_w = (int32_t)k_panel_width_px - ((int32_t)k_er_margin_x * 2);
  const int32_t body_h = (int32_t)k_panel_height_px - (int32_t)k_er_statusbar_h -
                         (int32_t)k_er_footer_h - ((int32_t)k_er_body_gap * 2);
  if ((body_w <= 0) || (body_h <= 0)) {
    return;
  }

  if (reflow_init((uint16_t)body_w,
                  (uint16_t)body_h,
                  g_ra8_font_literata_latin1,
                  (uint32_t)g_ra8_font_literata_latin1_len,
                  (uint16_t)k_er_reflow_px,
                  (uint32_t)k_er_reflow_ink,
                  (uint32_t)k_er_reflow_link,
                  &s_reflow_engine) != k_ra8_ok) {
    (void)internal_ns_log_emit("UI", "Reflow init failed");
    return;
  }

  s_reflow_img_arena = (ra8_img_arena_t){
    .base   = s_img_arena_buf,
    .cap    = (size_t)k_er_img_arena,
    .offset = 0U,
    .live   = 0U,
  };
  (void)reflow_set_image_loader(&s_reflow_engine,
                                internal_ns_image_loader,
                                nullptr,
                                &s_reflow_img_arena);

  uint32_t pages = 1U;
  if (reflow_layout_chapter(&s_reflow_engine,
                            (const uint8_t*)k_er_builtin_spine[0].xhtml,
                            k_er_builtin_spine[0].len,
                            &pages) == k_ra8_ok) {
    g_ns_total_pages     = (pages > 0U) ? pages : 1U;
    g_ns_current_page    = 0U;
    g_ns_current_chapter = 0U;
    g_ns_total_chapters  = 2U;
    s_reader_ready       = true;
    (void)internal_ns_log_emit("UI", "Reader subsystem ready");
  }
}

/** @brief Search and open book from SD library via streamed reader. */
RA8_INTERNAL static void internal_ns_open_sd_book(void)
{
  static const char* const k_candidate_books[] = {
    "Alice's Adventures in Wonderland - Lewis Carroll.epub",
    "alice.epub",
    "book.epub",
  };
  for (size_t i = 0U; i < (sizeof(k_candidate_books) / sizeof(k_candidate_books[0])); ++i) {
    if (epub_open_streamed_fs(s_sd_mount, k_candidate_books[i], &s_epub_io, &s_epub_book) ==
        k_ra8_ok) {
      uint16_t ch_count = 0U;
      if ((epub_get_chapter_count(&s_epub_book, &ch_count) == k_ra8_ok) && (ch_count > 0U)) {
        g_ns_total_chapters = (uint32_t)ch_count;
        g_ns_book_ready     = 1U;
        s_book_loaded       = true;
        (void)internal_ns_log_emit("SYS", "EPUB book opened from SD library");
        return;
      }
    }
  }
  (void)internal_ns_log_emit("SYS", "SD mounted; default reading library ready");
}

/** @brief SysTick exception handler forwarding to ThreadX kernel timer. */
extern void              PendSV_Handler(void);
extern void              _tx_timer_interrupt(void);
extern volatile uint32_t g_ra8_threadx_systick_ready;

RA8_INTERNAL static void internal_systick_handler(void)
{
  if (g_ra8_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/** @brief Low-power halt on fatal unrecoverable bring-up failure. */
RA8_INTERNAL [[noreturn]] static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Probe capacitive touch controller over TrustZone NSC. */
RA8_INTERNAL static void internal_ns_touch_setup(void)
{
  const ra8_nsc_touch_cfg_t tcfg = {
    .max_points = 1U,
    .irq_pin    = 32U,
  };
  ra8_err_t touch_err = k_ra8_ok;
  ER_NS_NSC_CALL(touch_err, ra8_nsc_board_touch_open(&tcfg));
  if (touch_err == k_ra8_ok) {
    g_ns_touch_ready = 1U;
    (void)internal_ns_log_emit("UI", "Touch controller open: OK");
  } else {
    g_ns_touch_ready = 0U;
    (void)internal_ns_log_emit("UI", "Touch controller open: skipped/unavailable");
  }
}

/** @brief Poll touchscreen, apply zoom/page-turn tap routing, and flag activity. */
RA8_INTERNAL static void internal_ns_poll_touch(uint32_t now_ms, bool* io_activity)
{
  if (g_ns_touch_ready == 0U) {
    return;
  }

  ra8_nsc_touch_point_t pt       = {};
  uint8_t               got      = 0U;
  ra8_err_t             read_err = k_ra8_ok;
  ER_NS_NSC_CALL(read_err, ra8_nsc_touch_read(&pt, 1U, &got));
  if ((read_err != k_ra8_ok) || (got == 0U)) {
    return;
  }

  g_ns_touch_events++;
  *io_activity = true;
  ra8_ereader_backlight_notify_activity();

  if (zoom_popup_active(&s_fig_popup)) {
    zoom_popup_action_t act = k_zoom_popup_act_none;
    if (zoom_popup_tap(&s_fig_popup, pt.x, pt.y, now_ms, &act) == k_ra8_ok) {
      g_ns_fig_scale = (uint32_t)s_fig_popup.view.scale;
      if (act == k_zoom_popup_act_redrawn) {
        internal_ns_figure_present();
      } else if (act == k_zoom_popup_act_closed) {
        (void)zoom_popup_close(&s_fig_popup);
        g_ns_fig_active = 0U;
        g_ns_fig_scale  = 0U;
        internal_ns_render_page();
      }
    }
    return;
  }

  if (!internal_ns_try_figure_tap(pt.x, pt.y)) {
    const er_dir_t dir = er_tap_to_dir(pt.x, (int32_t)k_panel_width_px);
    if (dir != k_er_dir_none) {
      internal_ns_handle_page_turn(dir);
    }
  }
}

/** @brief Step button/idle low-power logic and handle M33 hold/wake transitions. */
RA8_INTERNAL static void internal_ns_handle_lp_step(bool  sw1_edge,
                                                    bool  sw1_down,
                                                    bool  sw2_down,
                                                    bool* prev_sw1,
                                                    bool* prev_sw2,
                                                    bool  ui_activity)
{
  er_lp_input_action_t lp_action = k_er_lp_action_none;
  er_lowpower_ns_tick(sw1_down, sw2_down, (uint32_t)k_ns_ui_frame_ticks, ui_activity, &lp_action);

  const bool sw2_short = (lp_action == k_er_lp_action_sw2_short);
  if (s_ns_panel.initialized && (sw1_edge || sw2_short)) {
    const er_dir_t bdir = er_buttons_to_dir(sw1_edge, sw2_short);
    if (bdir != k_er_dir_none) {
      internal_ns_handle_page_turn(bdir);
    }
  }

  if (lp_action != k_er_lp_action_enter_hold) {
    return;
  }

  er_lp_wake_reason_t wake = k_er_lp_wake_none;
  const ra8_err_t     err  = er_lowpower_ns_handle_hold(s_ns_panel.frame_count,
                                                        (uint32_t)(uintptr_t)&s_framebuffer[0],
                                                        g_ns_fb_crc,
                                                        &wake);
  *prev_sw1                = false;
  *prev_sw2                = false;
  if ((err == k_ra8_ok) && (wake != k_er_lp_wake_none)) {
    /* Wake backlight on leaving low-power hold (button or soak wake). */
    ra8_ereader_backlight_notify_activity();
    if ((wake == k_er_lp_wake_next_page) || (wake == k_er_lp_wake_auto_test)) {
      if (s_ns_panel.initialized) {
        internal_ns_handle_page_turn(k_er_dir_next);
      }
      (void)internal_ns_log_emit("LP", "M85 wake -> page turn completed");
    } else if (wake == k_er_lp_wake_toggle) {
      (void)internal_ns_log_emit("LP", "M85 wake -> low-power toggle");
    }
  }
}

/** @brief UI thread entry: runs touch polling, page turns, zoom, and frame rendering. */
RA8_INTERNAL static void internal_ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)internal_ns_log_emit("UI", "UI thread started");
  internal_ns_panel_setup();
  internal_ns_backlight_setup(internal_ns_now_ms());
  internal_ns_touch_setup();
  internal_ns_reader_setup();
  internal_ns_render_page();
  er_lowpower_ns_setup((uint32_t)(uintptr_t)&s_framebuffer[0]);

  uint32_t frame    = 0U;
  bool     prev_sw1 = false;
  bool     prev_sw2 = false;
  for (;;) {
    frame++;
    g_ns_ui_frames = frame;

    bool sw1_edge = false;
    bool sw2_edge = false;
    bool sw1_down = false;
    bool sw2_down = false;
    internal_ns_backlight_tick(&prev_sw1, &prev_sw2, &sw1_edge, &sw2_edge, &sw1_down, &sw2_down);

    bool           ui_activity = sw1_edge || sw2_edge;
    const uint32_t now_ms      = internal_ns_now_ms();
    if (zoom_popup_active(&s_fig_popup) && zoom_popup_tick(&s_fig_popup, now_ms)) {
      internal_ns_figure_present();
    }

    internal_ns_poll_touch(now_ms, &ui_activity);
    internal_ns_handle_lp_step(sw1_edge, sw1_down, sw2_down, &prev_sw1, &prev_sw2, ui_activity);

    (void)ra8_wdt_supervisor_checkin(s_ui_wdt_handle);
    if ((frame % (uint32_t)k_ns_ui_heartbeat_frames) == 0U) {
      (void)internal_ns_log_emit("UI", "UI loop: frame heartbeat");
    }
    tx_thread_sleep((uint32_t)k_ns_ui_frame_ticks);
  }
}

/**
 * @brief System supervisor and storage background thread.
 */
RA8_INTERNAL static void internal_sys_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)internal_ns_log_emit("SYS", "System thread started (storage + supervisor)");

  /* Bring up SDHI pins and SD card */
  if (ra8_board_sdhi_pins_init() == k_ra8_ok) {
    const ra8_sdcard_cfg_t scfg = {.instance = 1U};
    if (ra8_sdcard_init(&scfg) == k_ra8_ok) {
      (void)ra8_io_vfs_init();
      if ((ra8_io_blockdev_sdhi_init(&s_sd_bd) == k_ra8_ok) &&
          (ra8_io_blockdev_as_fs_backend(&s_sd_bd, &s_sd_be) == k_ra8_ok) &&
          (ra8_fs_mount(&s_sd_be, &s_sd_mount) == k_ra8_ok)) {
        (void)ra8_io_vfs_mount("sd", s_sd_mount);
        g_ns_sd_ready = 1U;
        (void)internal_ns_log_emit("SYS", "SD storage mounted at sd:/");

        /* Attempt to open candidate EPUB from SD */
        internal_ns_open_sd_book();
      }
    }
  }

  uint32_t tick                  = 0U;
  bool     screen_off_lp_latched = false;
  for (;;) {
    tick++;
    g_ns_sys_ticks = tick;
    (void)ra8_wdt_supervisor_checkin(s_sys_wdt_handle);
    if ((tick % (uint32_t)k_ns_sys_heartbeat_iters) == 0U) {
      (void)internal_ns_log_emit("SYS", "System heartbeat: supervisor loop");
    }

    /* Screen-off idle and low-power entry (RA8FW-1400) */
    if (s_ns_backlight_ready && (g_ns_backlight_state == (uint32_t)k_display_backlight_state_off)) {
      if (!screen_off_lp_latched) {
        if (er_lowpower_ns_is_enabled()) {
          if (er_lowpower_ns_request_handoff() == k_ra8_ok) {
            screen_off_lp_latched = true;
            g_ns_idle_handoff_requests++;
            (void)internal_ns_log_emit("SYS", "Backlight OFF -> requested M33 hold handoff");
          }
        } else {
          /* M33 hold disabled or unavailable: enter timed Software Standby via Secure LPM */
          ra8_err_t lpm_err = k_ra8_ok;
          ER_NS_NSC_CALL(lpm_err, ra8_nsc_lpm_standby_ms(100U));
          if (lpm_err == k_ra8_ok) {
            screen_off_lp_latched = true;
            g_ns_lpm_standby_wakes++;
            (void)internal_ns_log_emit("SYS", "Backlight OFF -> LPM standby wake");
          }
        }
      }
    } else if (g_ns_backlight_state != (uint32_t)k_display_backlight_state_off) {
      screen_off_lp_latched = false;
    }

    tx_thread_sleep((uint32_t)k_ns_sys_poll_ticks);
  }
}

/** @brief Arm the watchdog and start the ThreadX check-in supervisor. */
RA8_INTERNAL static void internal_wdt_setup(void)
{
  ra8_err_t wdt_err = k_ra8_ok;
  ER_NS_NSC_CALL(wdt_err, ra8_nsc_wdt_start());
  if (wdt_err != k_ra8_ok) {
    internal_panic_halt();
  }
  const ra8_wdt_sup_cfg_t sup_cfg = {
    .stack             = s_sup_stack,
    .stack_size_bytes  = (uint32_t)k_ns_wdt_sup_stack_size,
    .priority          = (uint32_t)k_ns_wdt_sup_priority,
    .refresh_period_ms = (uint32_t)k_ns_wdt_refresh_ms,
  };
  if (ra8_wdt_supervisor_init(&sup_cfg) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_wdt_supervisor_set_refresh_hook(internal_ns_wdt_refresh) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_wdt_supervisor_register_thread("ui",
                                         (uint32_t)k_ns_wdt_ui_deadline_ms,
                                         &s_ui_wdt_handle) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_wdt_supervisor_register_thread("sys",
                                         (uint32_t)k_ns_wdt_sys_deadline_ms,
                                         &s_sys_wdt_handle) != k_ra8_ok) {
    internal_panic_halt();
  }
  if (ra8_wdt_supervisor_start() != k_ra8_ok) {
    internal_panic_halt();
  }
}

/** @brief Setup ThreadX threads and resources. */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;
  internal_wdt_setup();

  if (tx_thread_create(&s_ui_thread,
                       (CHAR*)(uintptr_t)"UI Thread",
                       internal_ui_thread_entry,
                       0UL,
                       s_ui_thread_stack,
                       k_ns_ui_thread_stack_size,
                       k_ns_ui_priority,
                       k_ns_ui_priority,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    internal_panic_halt();
  }

  if (tx_thread_create(&s_sys_thread,
                       (CHAR*)(uintptr_t)"System Thread",
                       internal_sys_thread_entry,
                       0UL,
                       s_sys_thread_stack,
                       k_ns_sys_thread_stack_size,
                       k_ns_sys_priority,
                       k_ns_sys_priority,
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    internal_panic_halt();
  }
}

/** @brief Non-Secure Reset handler: entered via Secure-to-NS transition. */
[[noreturn]] void ns_reset_handler(void);
[[noreturn]] void ns_reset_handler(void)
{
#ifdef RA8_EREADER_NS_XIP
  const uintptr_t data_src = (uintptr_t)&g_ra8_ls_ns_data_load_start;
  const uintptr_t data_dst = (uintptr_t)&g_ra8_ls_ns_data_start;
  const uintptr_t data_end = (uintptr_t)&g_ra8_ls_ns_data_end;
  for (uintptr_t off = 0U; (data_dst + off) < data_end; off += sizeof(uint32_t)) {
    *(volatile uint32_t*)(data_dst + off) = *(const volatile uint32_t*)(data_src + off);
  }
#endif

  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  *(volatile uint32_t*)k_ns_scb_vtor_addr = (uint32_t)(uintptr_t)&g_ra8_ls_ns_run_start;

  if (ra8_nsc_periph_init() != k_ra8_ok) {
    internal_panic_halt();
  }

  (void)internal_ns_log_emit("BOOT", "ra8d2-ereader: Non-Secure world online");
  tx_kernel_enter();
  internal_panic_halt();
}

typedef void (*ns_exc_handler_t)(void);

RA8_INTERNAL [[noreturn]] static void internal_nmi_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

[[gnu::section(".ns_vectors"), gnu::used]] const ns_exc_handler_t g_ra8_ns_vector_table[16] = {
  (ns_exc_handler_t)&g_ra8_ls_ns_stack_top, /* 0 Initial MSP_NS */
  ns_reset_handler,                         /* 1 Reset          */
  internal_nmi_halt,                        /* 2 NMI            */
  internal_nmi_halt,                        /* 3 HardFault      */
  internal_nmi_halt,                        /* 4 MemManage      */
  internal_nmi_halt,                        /* 5 BusFault       */
  internal_nmi_halt,                        /* 6 UsageFault     */
  internal_nmi_halt,                        /* 7 SecureFault    */
  0,                                        /* 8 Reserved       */
  0,                                        /* 9 Reserved       */
  0,                                        /* 10 Reserved      */
  internal_nmi_halt,                        /* 11 SVCall        */
  internal_nmi_halt,                        /* 12 DebugMonitor  */
  0,                                        /* 13 Reserved      */
  PendSV_Handler,                           /* 14 PendSV        */
  internal_systick_handler,                 /* 15 SysTick       */
};
