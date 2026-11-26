/**
 * @file apps/board/stand_alone/ereader/src/ns_main.c
 * @brief Non-Secure main entry point: launches ThreadX and the e-reader UI.
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
 * checking in within its deadline. If any thread wedges the supervisor stops
 * kicking the WDT and it underflows. Expiry is routed to NMI in this build, so
 * a wedge halts the system deterministically (context preserved for a
 * debugger) instead of hanging silently; routing expiry to an internal reset
 * for automatic reboot is a separate product decision.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdbool.h>
#include <stdint.h>

#include "er_panel.h"
#include "ra8_attributes.h"
#include "ra8_display_pal_backlight.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_io.h"
#include "ra8_panel.h"
#include "ra8_panel_timing.h"
#include "ra8_wdt_supervisor.h"
#include "tx_api.h"

/* Linker symbols for BSS and Stack */
extern uint32_t g_ra8_ls_ns_bss_start;
extern uint32_t g_ra8_ls_ns_bss_end;
extern uint32_t g_ra8_ls_ns_stack_top;
extern uint32_t g_ra8_ls_ns_run_start;

#ifdef RA8_EREADER_NS_XIP
/* XIP-only: .data lives (writable) in SRAM but its init image is in OSPI. These
 * mark the OSPI load image (LMA) and the SRAM run bounds (VMA) so the NS reset
 * handler can copy it in -- the Secure boot no longer copies the whole image. */
extern uint32_t g_ra8_ls_ns_data_load_start;
extern uint32_t g_ra8_ls_ns_data_start;
extern uint32_t g_ra8_ls_ns_data_end;
#endif

/** @brief Address of NS VTOR register. */
typedef enum : uintptr_t {
  k_ns_scb_vtor_addr = 0xE000ED08U, /**< Ns scb vtor address. */
} ns_scb_addr_t;

/**
 * @enum ns_thread_stack_t
 * @brief Stack sizes for Non-Secure ThreadX threads.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_thread_stack_size  = 4096U, /**< Stack size for UI thread.     */
  k_ns_sys_thread_stack_size = 2048U, /**< Stack size for System thread. */
} ns_thread_stack_t;

static TX_THREAD s_ui_thread;
static TX_THREAD s_sys_thread;

static uint8_t s_ui_thread_stack[k_ns_ui_thread_stack_size];
static uint8_t s_sys_thread_stack[k_ns_sys_thread_stack_size];

/**
 * @enum ns_log_cadence_t
 * @brief Heartbeat-log cadence per thread (iterations between log lines).
 * @details Each thread logs a startup line once, then a periodic heartbeat so
 *          the serial / SWO log shows the system is alive without flooding it.
 *          The UI thread sleeps ~16 ms (~60 FPS) and the system thread ~100
 *          ticks, so both cadences land near a one-second heartbeat.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_heartbeat_frames = 60U, /**< UI heartbeat every ~60 frames (~1 s).    */
  k_ns_sys_heartbeat_iters = 10U, /**< System heartbeat every ~10 loops (~1 s). */
} ns_log_cadence_t;

/**
 * @enum ns_thread_sleep_t
 * @brief Per-thread loop sleep period, in ThreadX ticks.
 * @details The UI thread sleeps ~16 ticks (~16 ms at a 1 kHz tick, ~60 FPS); the
 *          system thread sleeps ~100 ticks (~100 ms) since it only supervises.
 *          Every loop must sleep so the other threads get to run.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_frame_ticks = 16U,  /**< UI loop sleep (~16 ms, ~60 FPS).     */
  k_ns_sys_poll_ticks = 100U, /**< System loop sleep (~100 ms cadence). */
} ns_thread_sleep_t;

/**
 * @enum ns_thread_prio_t
 * @brief ThreadX priority + preemption threshold for each Non-Secure thread.
 * @details Lower numbers are higher priority. The UI thread runs ahead of the
 *          system thread so input/rendering stays responsive; each thread uses
 *          the same value for its priority and its preemption threshold.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_ns_ui_priority  = 10U, /**< UI thread priority + preemption threshold.     */
  k_ns_sys_priority = 15U, /**< System thread priority + preemption threshold. */
} ns_thread_prio_t;

/**
 * @enum ns_wdt_param_t
 * @brief Watchdog-supervisor sizing and timing parameters.
 * @details The supervisor refreshes the WDT every ::k_ns_wdt_refresh_ms only if
 *          both workers have checked in within their deadlines. The deadlines
 *          are generous multiples of each worker's loop period (UI ~16 ms, SYS
 *          ~100 ms) so ordinary slow work (a book load, an SD read) never false
 *          -trips the reset, while a genuine wedge is caught within the deadline.
 * @invariant Each deadline exceeds its worker's loop period by a wide margin.
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
 * @details Sits between the UI (::k_ns_ui_priority) and system
 *          (::k_ns_sys_priority) workers: high enough to preempt the background
 *          system thread and refresh on time, low enough not to starve the UI.
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
 * @var g_ns_ui_frames
 * @brief HIL liveness probe: UI-thread iteration count.
 * @details Bumped once per UI frame. Read via J-Link (NS SRAM survives a
 *          connect) to confirm the NS world is alive and the watchdog supervisor
 *          is being fed -- a frozen value means the UI thread wedged (and the
 *          WDT would then stop being refreshed). Diagnostic only.
 * @note Written by the UI thread; read externally by the bench.
 * @since 0.1.0
 */
volatile uint32_t g_ns_ui_frames = 0U;
/**
 * @var g_ns_sys_ticks
 * @brief HIL liveness probe: system-thread iteration count.
 * @details Bumped once per system-thread poll; see ::g_ns_ui_frames. A frozen
 *          value means the system thread wedged. Diagnostic only.
 * @note Written by the system thread; read externally by the bench.
 * @since 0.1.0
 */
volatile uint32_t g_ns_sys_ticks = 0U;
/**
 * @var g_ns_backlight_state
 * @brief Live NS backlight policy state (0 = ACTIVE, 1 = DIMMED, 2 = OFF).
 */
volatile uint32_t g_ns_backlight_state = (uint32_t)k_display_backlight_state_off;
/**
 * @var g_ns_backlight_pct
 * @brief Current target backlight brightness percent applied by NS UI thread (0..100).
 */
volatile uint32_t g_ns_backlight_pct = 0U;
/**
 * @var g_ns_backlight_transitions
 * @brief Total backlight state transitions observed by the NS UI loop.
 */
volatile uint32_t g_ns_backlight_transitions = 0U;
/**
 * @var g_ns_backlight_init_err
 * @brief Error status captured during NS backlight controller bring-up.
 */
volatile ra8_err_t g_ns_backlight_init_err = k_ra8_ok;
/**
 * @var g_ns_panel_ready
 * @brief Non-zero once NS display PAL + ``ra8_gfx`` initial frame flush succeeded.
 */
volatile uint32_t g_ns_panel_ready = 0U;
/**
 * @var g_ns_panel_init_err
 * @brief Error status captured during NS SDRAM + display PAL bring-up.
 */
volatile ra8_err_t g_ns_panel_init_err = k_ra8_ok;
/**
 * @var g_ns_refresh_mode
 * @brief Active selectable panel refresh mode (``er_refresh_mode_t``).
 */
volatile uint32_t g_ns_refresh_mode = (uint32_t)k_er_refresh_fast_clean;
/**
 * @var g_ns_refresh_hint
 * @brief Latest ``display_refresh_hint_t`` submitted to ``display_flush()``.
 */
volatile uint32_t g_ns_refresh_hint = (uint32_t)k_display_refresh_init;
/**
 * @var g_ns_frames_flushed
 * @brief Total frames rendered through ``ra8_gfx`` and flushed via display PAL.
 */
volatile uint32_t g_ns_frames_flushed = 0U;
/**
 * @var g_ns_fb_crc
 * @brief FNV-1a checksum across the SDRAM panel framebuffer after the latest flush.
 */
volatile uint32_t g_ns_fb_crc = 0U;

/** @brief 1024x600 RGB565 panel framebuffer placed in NOLOAD external SDRAM. */
RA8_BOARD_PANEL_FRAMEBUFFER(s_framebuffer);

static er_panel_state_t           s_ns_panel                   = {};
static display_backlight_t        s_ns_backlight               = {};
static display_backlight_policy_t s_ns_backlight_policy        = {};
static ra8_nsc_backlight_caps_t   s_ns_backlight_caps          = {};
static bool                       s_ns_backlight_ready         = false;
static volatile bool              s_backlight_activity_pending = false;

/**
 * @brief Convert ThreadX kernel ticks into monotonic milliseconds.
 * @return Elapsed milliseconds from ThreadX tick counter.
 */
RA8_INTERNAL static uint32_t internal_ns_now_ms(void)
{
  return (uint32_t)(((uint64_t)tx_time_get() * 1000ULL) / (uint64_t)TX_TIMER_TICKS_PER_SECOND);
}

/**
 * @brief Backlight enable callback bridged across TrustZone to Secure board GPIO.
 * @param[in] ctx Unused caller context.
 * @param[in] on  1 for ON, 0 for OFF.
 * @return ``k_ra8_ok`` on success or NSC error code.
 */
RA8_INTERNAL static ra8_err_t internal_ns_backlight_set_cb(void* ctx, uint8_t on)
{
  (void)ctx;
  return ra8_nsc_backlight_set(on != 0U);
}

/**
 * @brief Signal user or display activity to wake the NS backlight idle timer.
 * @details Lockless single-flag latch consumed by the NS UI thread.
 * @since 0.1.0
 */
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
  ra8_err_t err = ra8_nsc_sdramc_init();
  if (err != k_ra8_ok) {
    g_ns_panel_init_err = err;
    (void)ra8_nsc_log_emit("UI", "Panel SDRAM init failed");
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
    (void)ra8_nsc_log_emit("UI", "Panel display PAL init failed");
    return;
  }

  err = er_panel_render_frame(&s_ns_panel,
                              k_display_event_open,
                              "RA8D2 E-Reader",
                              "Panel ready through ra8_display_pal + ra8_gfx");
  if (err != k_ra8_ok) {
    g_ns_panel_init_err = err;
    (void)ra8_nsc_log_emit("UI", "Panel initial frame flush failed");
    return;
  }

  g_ns_panel_init_err = k_ra8_ok;
  g_ns_panel_ready    = 1U;
  internal_ns_panel_publish();
  (void)ra8_nsc_log_emit("UI", "Panel initialized: frame 1 flushed");
  (void)ra8_nsc_log_emit("UI", er_panel_refresh_mode_label(s_ns_panel.mode));
}

/**
 * @brief Bring up the NS backlight controller, 30s idle policy, and wake switches.
 * @param[in] now_ms Monotonic timestamp in milliseconds.
 */
RA8_INTERNAL static void internal_ns_backlight_setup(uint32_t now_ms)
{
  ra8_err_t err = ra8_nsc_backlight_get_caps(&s_ns_backlight_caps);
  if (err != k_ra8_ok) {
    g_ns_backlight_init_err = err;
    return;
  }
  if (g_ns_panel_ready == 0U) {
    err = ra8_nsc_backlight_init(false);
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

  const uint8_t                        supports_dim = s_ns_backlight_caps.supports_pwm ? 1U : 0U;
  const display_backlight_policy_cfg_t pol_cfg      = {
    .dim_after_ms = (supports_dim != 0U) ? (uint32_t)k_display_backlight_default_dim_ms : 0U,
    .off_after_ms = (uint32_t)k_display_backlight_default_off_ms,
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

  (void)ra8_nsc_sw_init(0U);
  (void)ra8_nsc_sw_init(1U);

  g_ns_backlight_init_err = k_ra8_ok;
  g_ns_backlight_state    = (uint32_t)s_ns_backlight_policy.state;
  g_ns_backlight_pct      = (uint32_t)s_ns_backlight.brightness_pct;
  s_ns_backlight_ready    = true;
}

/**
 * @brief Pump user button edges and advance the 30s backlight idle policy.
 * @param[in,out] prev_sw1 Prior sampled SW1 pressed state.
 * @param[in,out] prev_sw2 Prior sampled SW2 pressed state.
 */
RA8_INTERNAL static void
internal_ns_backlight_tick(bool* prev_sw1, bool* prev_sw2, bool* out_sw1_edge, bool* out_sw2_edge)
{
  *out_sw1_edge = false;
  *out_sw2_edge = false;
  if (!s_ns_backlight_ready) {
    return;
  }

  bool sw1_now = false;
  bool sw2_now = false;
  (void)ra8_nsc_sw_read(0U, &sw1_now);
  (void)ra8_nsc_sw_read(1U, &sw2_now);

  const bool sw1_edge = sw1_now && !(*prev_sw1);
  const bool sw2_edge = sw2_now && !(*prev_sw2);
  const bool sw_edge  = sw1_edge || sw2_edge;
  *prev_sw1           = sw1_now;
  *prev_sw2           = sw2_now;
  *out_sw1_edge       = sw1_edge;
  *out_sw2_edge       = sw2_edge;

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
      (void)ra8_nsc_log_emit("UI", (dec.on != 0U) ? "Backlight: active" : "Backlight: idle off");
    }
  }
}

/**
 * @brief Perform the ThreadX Non-Secure context switch requested by PendSV.
 * @details The ThreadX Cortex-M port supplies this exception entry point. The
 *          Non-Secure vector table references it directly so a pending context
 *          switch saves the outgoing thread and restores the scheduler-selected
 *          thread before exception return.
 * @pre PendSV was entered in Non-Secure handler mode.
 * @pre ThreadX has initialised the scheduler and thread contexts.
 * @post The pending context-switch request has been serviced.
 * @post The scheduler-selected thread context is active for exception return.
 * @note Exception-context entry point supplied by ThreadX; not thread-callable.
 * @since 0.1.0
 */
extern void PendSV_Handler(void);

/**
 * @brief Advance the ThreadX kernel timer by one hardware tick.
 * @details The ThreadX Cortex-M port supplies this timer ISR worker. The local
 *          SysTick handler calls it only after the kernel readiness flag is set,
 *          allowing ThreadX delays, timers, and timed waits to advance.
 * @pre ThreadX low-level initialisation has completed.
 * @pre Called once for the SysTick event being serviced.
 * @post The ThreadX system clock has advanced by one tick.
 * @post Any timeout made ready by that tick is visible to the scheduler.
 * @note Runs in Non-Secure handler mode and is not re-entrant.
 * @since 0.1.0
 */
extern void _tx_timer_interrupt(void);

/* External state for the thread tick hook. */
extern volatile uint32_t g_ra8_threadx_systick_ready;

/**
 * @brief SysTick exception handler for the Non-Secure ThreadX OS.
 * @details Drives the ThreadX timer: once the kernel is up
 *          (@c g_ra8_threadx_systick_ready set), each SysTick forwards into
 *          @c _tx_timer_interrupt to service timeouts and the time-slice. Before
 *          the kernel is ready the tick is ignored so an early SysTick cannot
 *          enter the scheduler.
 * @return Nothing.
 * @pre Installed as the SysTick vector in the NS vector table.
 * @pre @c g_ra8_threadx_systick_ready is set only after @c tx_kernel_enter.
 * @post When the kernel is ready, one ThreadX timer tick has been serviced.
 * @post When the kernel is not ready, no scheduler state is touched.
 * @note Runs in NS handler mode; not callable from thread context.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_systick_handler(void)
{
  if (g_ra8_threadx_systick_ready != 0U) {
    _tx_timer_interrupt();
  }
}

/**
 * @brief Park the CPU in an idle loop if a fatal startup error occurs.
 * @details Spins forever issuing @c wfi so a Non-Secure bring-up failure halts
 *          deterministically (low power) instead of executing undefined state.
 *          Recovery requires an external reset.
 * @return Does not return (@c noreturn).
 * @note The function never returns to its caller.
 * @pre Called only on an unrecoverable NS startup error.
 * @pre Interrupts that could resume normal flow are not relied upon.
 * @post The core remains parked in a low-power wait loop until reset.
 * @post No further application code executes.
 * @note Not thread-safe; terminal error path only.
 * @since 0.1.0
 */
RA8_INTERNAL [[noreturn]] static void internal_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief UI thread entry: runs the e-reader UI frame loop.
 * @details The Non-Secure UI thread. By the time ThreadX schedules it the Secure
 *          substrate, the TrustZone boundary, and logging (SCI8) are all up, so it
 *          drives the frame loop for the lifetime of the system: each iteration
 *          advances the frame counter, emits a heartbeat on the configured
 *          cadence, and yields so the other threads run. Never returns.
 * @param[in] thread_input ThreadX entry argument (unused; reserved by the API).
 * @return Nothing (runs for the lifetime of the system).
 * @pre Registered as the UI thread's entry in ::tx_application_define.
 * @pre @c ra8_nsc_periph_init has completed (Secure services available).
 * @post The UI loop runs continuously, yielding each iteration.
 * @post Heartbeat log lines are emitted on the configured cadence.
 * @note Runs on the NS UI thread; not thread-safe across threads.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_ui_thread_entry(ULONG thread_input)
{
  (void)thread_input;
  (void)ra8_nsc_log_emit("UI", "UI thread started");
  internal_ns_panel_setup();
  internal_ns_backlight_setup(internal_ns_now_ms());

  uint32_t frame    = 0U;
  bool     prev_sw1 = false;
  bool     prev_sw2 = false;
  for (;;) {
    frame++;
    g_ns_ui_frames = frame; /* HIL liveness probe (read via J-Link). */
    bool sw1_edge  = false;
    bool sw2_edge  = false;
    internal_ns_backlight_tick(&prev_sw1, &prev_sw2, &sw1_edge, &sw2_edge);
    if (s_ns_panel.initialized) {
      if (sw2_edge) {
        if (er_panel_cycle_refresh_mode(&s_ns_panel) == k_ra8_ok) {
          (void)er_panel_render_frame(&s_ns_panel,
                                      k_display_event_turn,
                                      "RA8D2 E-Reader",
                                      "Refresh mode switched by user input");
          internal_ns_panel_publish();
          (void)ra8_nsc_log_emit("UI", er_panel_refresh_mode_label(s_ns_panel.mode));
        }
      } else if (sw1_edge) {
        if (er_panel_render_frame(&s_ns_panel,
                                  k_display_event_turn,
                                  "RA8D2 E-Reader",
                                  "Page turn flushed via display PAL") == k_ra8_ok) {
          internal_ns_panel_publish();
        }
      }
    }
    /* Prove liveness to the watchdog supervisor once per frame. */
    (void)ra8_wdt_supervisor_checkin(s_ui_wdt_handle);
    if ((frame % (uint32_t)k_ns_ui_heartbeat_frames) == 0U) {
      (void)ra8_nsc_log_emit("UI", "UI loop: frame heartbeat");
    }
    tx_thread_sleep((uint32_t)k_ns_ui_frame_ticks); /* ~16 ms; yields to the others. */
  }
}

/**
 * @brief System supervisor and storage background thread.
 * @details Lower-priority background worker for long-running, non-UI duties --
 *          SD card / filesystem setup, input polling, battery status, and OTA
 *          state. It runs an infinite supervisor loop, emitting a periodic
 *          heartbeat and sleeping each iteration so the UI thread stays
 *          responsive. It never returns.
 * @param[in] thread_input ThreadX entry argument (unused; reserved by the API).
 * @return Nothing (runs for the lifetime of the system).
 * @pre Registered as the system thread's entry in ::tx_application_define.
 * @pre ThreadX is running (the scheduler invoked this entry).
 * @post The supervisor loop runs continuously, yielding each iteration.
 * @post Heartbeat log lines are emitted on the configured cadence.
 * @note Runs on the NS system thread; not thread-safe across threads.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_sys_thread_entry(ULONG thread_input)
{
  (void)thread_input;

  (void)ra8_nsc_log_emit("SYS", "System thread started (storage + supervisor)");
  uint32_t tick = 0U;
  for (;;) {
    tick++;
    g_ns_sys_ticks = tick; /* HIL liveness probe (read via J-Link). */
    /* Prove liveness to the watchdog supervisor once per poll. */
    (void)ra8_wdt_supervisor_checkin(s_sys_wdt_handle);
    if ((tick % (uint32_t)k_ns_sys_heartbeat_iters) == 0U) {
      (void)ra8_nsc_log_emit("SYS", "System heartbeat: supervisor loop");
    }
    tx_thread_sleep((uint32_t)k_ns_sys_poll_ticks);
  }
}

/**
 * @brief Arm the watchdog and start the ThreadX check-in supervisor.
 * @details Runs once from ::tx_application_define, before the workers are
 *          created, and in this exact order: arm the Secure WDT via the
 *          ``ra8_nsc_wdt_start`` veneer; initialise the supervisor with a
 *          caller-owned stack; redirect its refresh hook to the
 *          ``ra8_nsc_wdt_refresh`` veneer (the library default would call the
 *          WDT directly, which faults in NS); register the UI and system
 *          workers with their deadlines; then start the supervisor thread. Any
 *          failure is unrecoverable this early, so it parks in ::internal_panic_halt.
 * @return Nothing.
 * @note Returns only on full success; otherwise never returns (halts).
 * @pre ::ra8_nsc_periph_init has completed (Secure clocks + substrate up).
 * @pre Called in single-threaded boot context before any worker is created.
 * @post The WDT is armed and the supervisor thread is running.
 * @post ::s_ui_wdt_handle and ::s_sys_wdt_handle hold valid registry handles.
 * @note Not thread-safe; boot-time only.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_wdt_setup(void)
{
  if (ra8_nsc_wdt_start() != k_ra8_ok) {
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
  /* Route refresh through the NSC veneer -- the WDT is Secure-owned, so the
   * library's default direct ra8_wdt_refresh_deferred would fault in NS. */
  if (ra8_wdt_supervisor_set_refresh_hook(ra8_nsc_wdt_refresh) != k_ra8_ok) {
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

/**
 * @brief Setup ThreadX threads and resources.
 */
void tx_application_define(void* first_unused_memory)
{
  (void)first_unused_memory;

  /* Arm the WDT + start the check-in supervisor before any worker runs. */
  internal_wdt_setup();

  /* Create the UI thread. A failed create is unrecoverable this early: the
   * WDT supervisor already registered the "ui"/"sys" deadlines, so a missing
   * worker would only surface later as a silent watchdog boot-loop -- panic
   * now instead, like every other NS bring-up failure in this file. */
  /* (CHAR*)(uintptr_t): the vendored ThreadX API takes a non-const CHAR* for
   * the thread name; the uintptr_t hop launders the string-literal const
   * without tripping -Wcast-qual (same pattern as ra8_wdt_supervisor). */
  if (tx_thread_create(&s_ui_thread,
                       (CHAR*)(uintptr_t)"UI Thread",
                       internal_ui_thread_entry,
                       0UL,
                       s_ui_thread_stack,
                       k_ns_ui_thread_stack_size,
                       k_ns_ui_priority, /* Priority             */
                       k_ns_ui_priority, /* Preemption threshold */
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    internal_panic_halt();
  }

  /* Create the System/Storage thread */
  if (tx_thread_create(&s_sys_thread,
                       (CHAR*)(uintptr_t)"System Thread",
                       internal_sys_thread_entry,
                       0UL,
                       s_sys_thread_stack,
                       k_ns_sys_thread_stack_size,
                       k_ns_sys_priority, /* Priority             */
                       k_ns_sys_priority, /* Preemption threshold */
                       TX_NO_TIME_SLICE,
                       TX_AUTO_START) != TX_SUCCESS) {
    internal_panic_halt();
  }
}

/**
 * @brief Non-Secure Reset handler: entered via Secure-to-NS transition.
 *
 * @details
 * Slot 1 of the NS vector table and the NS image's linker entry symbol; the
 * Secure boot BLXNS-es here. No header declares it because the only callers
 * are the hardware vector fetch and the linker -- the prototype below
 * satisfies -Wmissing-prototypes for this externally-linked boot symbol.
 *
 * @pre The Secure boot copied/armed the NS image and programmed VTOR_NS.
 * @pre Executes in NS Thread mode with MSP_NS from vector slot 0.
 * @post .bss (and, under XIP, .data) is initialised; ThreadX never returns.
 * @post Interrupt state is whatever tx_kernel_enter establishes.
 *
 * @note Not thread-safe; single-threaded NS boot only.
 * @since 0.1.0
 */
[[noreturn]] void ns_reset_handler(void);

[[noreturn]] void ns_reset_handler(void)
{
#ifdef RA8_EREADER_NS_XIP
  /* XIP: code/rodata execute in place from OSPI, but .data must be writable, so
   * copy its init image from OSPI (LMA) into SRAM (VMA) before any initialised
   * global is read. Must run first -- nothing below may touch a .data global. */
  const uintptr_t data_src = (uintptr_t)&g_ra8_ls_ns_data_load_start;
  const uintptr_t data_dst = (uintptr_t)&g_ra8_ls_ns_data_start;
  const uintptr_t data_end = (uintptr_t)&g_ra8_ls_ns_data_end;
  for (uintptr_t off = 0U; (data_dst + off) < data_end; off += sizeof(uint32_t)) {
    *(volatile uint32_t*)(data_dst + off) = *(const volatile uint32_t*)(data_src + off);
  }
#endif

  /* Zero the NS BSS section */
  const uintptr_t bss_start = (uintptr_t)&g_ra8_ls_ns_bss_start;
  const uintptr_t bss_end   = (uintptr_t)&g_ra8_ls_ns_bss_end;
  for (uintptr_t addr = bss_start; addr < bss_end; addr += sizeof(uint32_t)) {
    *(volatile uint32_t*)addr = 0U;
  }

  /* Set the NS VTOR so exceptions vector correctly to NS handlers */
  *(volatile uint32_t*)k_ns_scb_vtor_addr = (uint32_t)(uintptr_t)&g_ra8_ls_ns_run_start;

  /* Call Secure-side substrate initialization via NSC veneer gateway */
  if (ra8_nsc_periph_init() != k_ra8_ok) {
    internal_panic_halt();
  }

  /* Announce the Non-Secure world is live before handing off to ThreadX.
   * Logs flow NS -> ra8_nsc_log_emit veneer -> Secure ra8_log_info -> ITM
   * (visible on the J-Link SWO console). */
  (void)ra8_nsc_log_emit("BOOT", "ra8d2-ereader: Non-Secure world online");

  /* Enter ThreadX RTOS kernel (never returns) */
  tx_kernel_enter();

  internal_panic_halt();
}

/* =============================================================================
 * Non-Secure Vector Table
 * =============================================================================
 */
typedef void (*ns_exc_handler_t)(void);

/**
 * @brief Default Non-Secure fault/NMI handler: park the CPU in a wfi halt.
 * @details Installed on the NMI, HardFault, and the other unhandled Non-Secure
 *          exception vectors. There is no recovery path, so it spins in a @c wfi
 *          loop and leaves the faulting context intact for a debugger.
 * @return Nothing (noreturn; control never leaves the halt loop).
 * @note This function does not return.
 * @pre Reached only via a Non-Secure exception vector.
 * @pre Recovery, if any, is the debugger's or watchdog's responsibility.
 * @post The CPU is parked in a wfi loop and runs no further NS code.
 * @post The faulting context is left unmodified for inspection.
 * @note Runs in NS exception context.
 * @since 0.1.0
 */
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
