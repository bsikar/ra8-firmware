/**
 * @file examples/ek_ra8d2/hw_validated/hil/app_launch_demo/main.c
 * @brief Minimal chrome stub: register apps + launch + back-stack (#146).
 *
 * @details
 * The first runnable increment of the app framework (#146). It proves the
 * `ra8_app` registry + launcher + the navigation back-stack (`ra8_app_nav_t`)
 * end-to-end on the real Cortex-M85 image -- no display, no widgets, just the
 * lifecycle and routing so the path is observable headlessly on `ra8_emulator`
 * via the ITM log (each line shows up as `[itm] ...`).
 *
 * What it does, acting as a tiny "chrome" / shell:
 *   1. Registers two stub apps into one registry:
 *      - `reader`   (id 1) -- a **core, non-removable** app (`removable=false`):
 *        the "core functionality should be able to be uninstalled" point from
 *        #146 says the *optional* apps are removable; the reader is core here.
 *      - `settings` (id 2) -- an **optional, removable** app (`removable=true`),
 *        wrapped in a build-time guard (`#if APP_LAUNCH_SETTINGS`). Building with
 *        `-DAPP_LAUNCH_SETTINGS=0` drops it from the registry entirely -- the
 *        "core uninstallable" build-time-exclusion mechanism -- and the demo
 *        still builds + runs (it just skips the switch/back leg).
 *   2. Launches `reader` from the chrome through `ra8_app_nav_go` (first focus,
 *      nothing pushed) and renders it.
 *   3. Switches to `settings` through `ra8_app_nav_go` -- the focus lifecycle
 *      fires (`reader.on_leave` -> `settings.on_enter`) and `reader` is pushed
 *      onto the back-stack.
 *   4. Presses "back" through `ra8_app_nav_back` -- `settings` leaves, `reader`
 *      re-enters, and the back-stack empties.
 *
 * Each stub app is a function-pointer vtable (init / on_enter / tick / render /
 * on_input / on_leave / deinit) plus the `removable` flag; the stubs just count
 * their lifecycle calls and log them. A deterministic self-check asserts the
 * exact call counts and back-stack depth at every step, then emits:
 *
 *   `[app_launch] INFO: demo PASS`  (ra8_emulator: `[itm] [app_launch] INFO: demo PASS`)
 *
 * so the app doubles as a `ra8_emulator` regression gate; any failure logs a
 * `FAIL ...` line and parks in WFI.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * [Ring 6 / APP] {World: S}
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_app.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_widget.h"

/**
 * @enum app_launch_hil_t
 * @brief VCOM-console line rate for the deterministic HIL success banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate; the Pi HIL rig's `uart_scrape` reads /dev/ttyACM0 to gate the
 *          app. The banner is additive to the existing `ra8_log` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_app_launch_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} app_launch_hil_t;

/**
 * @var k_app_launch_pass_banner
 * @brief Deterministic, run-to-run-stable HIL success banner (uart_scrape gate).
 * @details Emitted over the SCI8 / J-Link OB VCOM console only on the success
 *          path, AFTER the registry + launcher self-check passes. The ITM
 *          `ra8_log` verdict is left intact; this is purely additive so the Pi
 *          rig (which has no SWO / ITM capture) can gate the app.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; the HIL gate (hil.conf HIL_EXPECT) matches it exactly.
 * @since 0.1.0
 */
static const uint8_t k_app_launch_pass_banner[] = "app_launch_demo: demo PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL success banner.
 *
 * @details
 * Configures the clock tree (`ra8_cgc_init`, which publishes the PCLKA the SCI8
 * BRR divisor is computed from) then the EK-RA8D2 debug console
 * (`ra8_board_uart_console_init`, SCI8 on PD02/PD03 at ::k_app_launch_hil_baud).
 * Best-effort: a failure only means the additive HIL banner cannot reach the
 * host; the existing `ra8_log` ITM trace and the demo logic are unaffected.
 *
 * @return Whether the VCOM console is ready to carry the banner.
 * @retval true  Clock + SCI8 console are up.
 * @retval false A bring-up step failed (the banner is then silently skipped).
 *
 * @pre Called once during bring-up, before the success banner is emitted.
 * @pre `ra8_log_init` has run (failures are narrated over ITM).
 * @post On true, SCI8 is enabled (TE/RE) and PD02/PD03 route to it.
 * @post On false, no console state persists; the app continues normally.
 *
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static bool app_launch_hil_console_init(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_app_launch_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console.
 *
 * @details
 * Writes ::k_app_launch_pass_banner to the SCI8 / J-Link OB VCOM console and
 * flushes it so the bytes clock out before the CPU parks in WFI. A no-op if the
 * console never came up (the write returns `k_ra8_err_not_initialized`, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only on the verified success path (single, non-compound guard).
 * @pre ::app_launch_hil_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No registry / app state is modified.
 *
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static void app_launch_hil_emit_pass(void)
{
  (void)ra8_board_uart_console_write(k_app_launch_pass_banner,
                                     (size_t)(sizeof(k_app_launch_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
}

/**
 * @def APP_LAUNCH_SETTINGS
 * @brief Build-time selector for the optional `settings` app (#146 exclusion).
 *
 * @details
 * Defaults to 1 (settings shipped). Define it to 0 at configure time
 * (`-DAPP_LAUNCH_SETTINGS=0`) to exclude the removable `settings` app from the
 * registry, demonstrating the "core uninstallable" build-time-exclusion path:
 * the firmware still builds and runs, with `settings` simply absent.
 *
 * @note Compile-time only; no run-time cost.
 * @since 0.1.0
 */
#ifndef APP_LAUNCH_SETTINGS
/** @brief APP LAUNCH SETTINGS. */
#define APP_LAUNCH_SETTINGS (1)
#endif

/**
 * @enum app_launch_id_t
 * @brief Registered app ids (launch keys).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_app_id_reader   = 1U, /**< Core EPUB-reader app (non-removable). */
  k_app_id_settings = 2U, /**< Optional settings app (removable).    */
} app_launch_id_t;

/**
 * @enum app_launch_const_t
 * @brief Capacities + the button id the stub apps understand.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_app_reg_cap  = 2U, /**< Registry storage slots (reader + settings).  */
  k_app_nav_cap  = 4U, /**< Back-stack depth (history of focused apps).  */
  k_app_btn_back = 1U, /**< Button id a chrome routes to mean "go back". */
} app_launch_const_t;

/**
 * @struct app_launch_ctx_t
 * @brief Per-app stub state: a display name + lifecycle-call counters.
 *
 * @details
 * Each registered app points its `ctx` at one of these. The vtable callbacks
 * bump the matching counter and log the event, so the self-check can assert the
 * exact focus lifecycle (how many enters/leaves/renders fired) without any
 * display.
 *
 * @invariant `enters >= leaves` while the app is foreground.
 * @since 0.1.0
 */
typedef struct {
  const char* name;    /**< Display name + ITM log tag (non-NULL). */
  uint32_t    enters;  /**< Times `on_enter` fired.                */
  uint32_t    leaves;  /**< Times `on_leave` fired.                */
  uint32_t    renders; /**< Times `render` fired.                  */
  uint32_t    inputs;  /**< Times `on_input` fired.                */
} app_launch_ctx_t;

/**
 * @var s_reg
 * @brief The app registry (single active app + membership table).
 * @note Mutated only from `main` (single-threaded init context).
 * @warning Do not access from interrupt context.
 * @since 0.1.0
 */
static ra8_app_registry_t s_reg;

/**
 * @var s_slots
 * @brief Caller-owned storage backing the registry's app-pointer table.
 * @note Bound to ::s_reg by `ra8_app_registry_init`.
 * @warning Sized to ::k_app_reg_cap; never indexed past `s_reg.count`.
 * @since 0.1.0
 */
static ra8_app_t* s_slots[k_app_reg_cap];

/**
 * @var s_nav_stack
 * @brief Caller-owned storage backing the navigation back-stack (app ids).
 * @note Bound to ::s_nav by `ra8_app_nav_init`.
 * @warning Sized to ::k_app_nav_cap.
 * @since 0.1.0
 */
static uint16_t s_nav_stack[k_app_nav_cap];

/**
 * @var s_nav
 * @brief Navigation back-stack layered over ::s_reg.
 * @note Mutated only from `main`.
 * @warning Drives ::s_reg; keep their lifetimes together.
 * @since 0.1.0
 */
static ra8_app_nav_t s_nav;

/**
 * @var s_reader_ctx
 * @brief Stub state for the core `reader` app.
 * @note Read/written by the reader app's vtable callbacks only.
 * @warning Counters are asserted by the self-check; do not reset elsewhere.
 * @since 0.1.0
 */
static app_launch_ctx_t s_reader_ctx = {.name = "reader"};

/**
 * @var s_reader_app
 * @brief The core `reader` app instance (registered into ::s_reg).
 * @note Built in `app_launch_register`.
 * @warning Must outlive the registry (static storage).
 * @since 0.1.0
 */
static ra8_app_t s_reader_app;

#if APP_LAUNCH_SETTINGS
/**
 * @var s_settings_ctx
 * @brief Stub state for the optional `settings` app.
 * @note Compiled only when ::APP_LAUNCH_SETTINGS is enabled.
 * @warning Counters are asserted by the self-check.
 * @since 0.1.0
 */
static app_launch_ctx_t s_settings_ctx = {.name = "settings"};

/**
 * @var s_settings_app
 * @brief The optional `settings` app instance (registered into ::s_reg).
 * @note Compiled only when ::APP_LAUNCH_SETTINGS is enabled.
 * @warning Must outlive the registry (static storage).
 * @since 0.1.0
 */
static ra8_app_t s_settings_app;
#endif

/* ===========================================================================
 * Stub-app vtable callbacks (one shared vtable; behaviour keyed off ctx)
 * ===========================================================================
 */

/**
 * @brief Stub `on_enter`: count the focus gain and log it.
 * @param[in,out] a The app gaining focus (non-NULL, `ctx` is an app_launch_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @post The app's `enters` counter advanced by one.
 * @post One `[name] INFO: on_enter` line is emitted (or dropped if no ITM).
 * @note Not thread-safe; called from the foreground switch path.
 * @since 0.1.0
 */
static void app_stub_on_enter(ra8_app_t* a)
{
  app_launch_ctx_t* c = (app_launch_ctx_t*)a->ctx;
  c->enters++;
  ra8_log_info(c->name, "on_enter");
}

/**
 * @brief Stub `on_leave`: count the focus loss and log it.
 * @param[in,out] a The app losing focus (non-NULL, `ctx` is an app_launch_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @post The app's `leaves` counter advanced by one.
 * @post One `[name] INFO: on_leave` line is emitted (or dropped).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_stub_on_leave(ra8_app_t* a)
{
  app_launch_ctx_t* c = (app_launch_ctx_t*)a->ctx;
  c->leaves++;
  ra8_log_info(c->name, "on_leave");
}

/**
 * @brief Stub `render`: count the frame and log it (stands in for drawing).
 * @param[in] a The foreground app (non-NULL, `ctx` is an app_launch_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @post The app's `renders` counter advanced by one.
 * @post One `[name] INFO: render` line is emitted (or dropped).
 * @note Not thread-safe; the real on-target draw callback would live here.
 * @since 0.1.0
 */
static void app_stub_render(const ra8_app_t* a)
{
  app_launch_ctx_t* c = (app_launch_ctx_t*)a->ctx;
  c->renders++;
  ra8_log_info(c->name, "render");
}

/**
 * @brief Stub `on_input`: count the event, consume only the "back" button.
 * @param[in,out] a  The foreground app (non-NULL, `ctx` is an app_launch_ctx_t).
 * @param[in]     ev The input event (non-NULL).
 * @return true if the event was the back button (consumed), false otherwise.
 * @retval true  `ev` is a ::k_app_btn_back button press.
 * @retval false Any other event (the chrome keeps routing it).
 * @pre `a`, `a->ctx`, and `ev` are non-NULL.
 * @post The app's `inputs` counter advanced by one.
 * @post On a back press one `[name] INFO: back requested` line is emitted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_stub_on_input(ra8_app_t* a, const ra8_widget_event_t* ev)
{
  app_launch_ctx_t* c = (app_launch_ctx_t*)a->ctx;
  c->inputs++;
  if (ev->kind != k_ra8_widget_ev_button) {
    return false;
  }
  if (ev->button_id != (uint16_t)k_app_btn_back) {
    return false;
  }
  ra8_log_info(c->name, "back requested");
  return true;
}

/**
 * @var k_app_stub_vt
 * @brief Shared lifecycle vtable for every stub app (behaviour keyed off ctx).
 * @details `init`, `tick`, and `deinit` are unused here (NULL = no-op), proving
 *          the framework tolerates partial vtables.
 * @note Read-only; lives in .rodata.
 * @since 0.1.0
 */
static const ra8_app_vtable_t k_app_stub_vt = {
  .init     = nullptr,
  .on_enter = app_stub_on_enter,
  .tick     = nullptr,
  .render   = app_stub_render,
  .on_input = app_stub_on_input,
  .on_leave = app_stub_on_leave,
  .deinit   = nullptr,
};

/* ===========================================================================
 * Registration + self-check (the "chrome")
 * ===========================================================================
 */

/**
 * @brief Initialise the registry + back-stack and register every app.
 *
 * @details
 * Binds ::s_reg and ::s_nav to their caller-owned storage, builds the core
 * `reader` app (non-removable) and -- when ::APP_LAUNCH_SETTINGS is on -- the
 * optional `settings` app (removable), and registers each. Any failure short-
 * circuits to a false return so `main` can log and park.
 *
 * @return true if the registry, back-stack, and all apps initialised.
 * @retval true  Every `ra8_app_*_init` / `ra8_app_register` returned k_ra8_ok.
 * @retval false Some init/register step failed.
 *
 * @pre Called once from `main` before any launch.
 * @pre The static app/ctx storage is zero-initialised (C startup).
 * @post On true, ::s_reg holds the shipped apps and ::s_nav is empty.
 * @post On false, ::s_reg may be partially populated (the caller parks).
 *
 * @note Not thread-safe (single-threaded init context).
 * @since 0.1.0
 */
static bool app_launch_register(void)
{
  if (ra8_app_registry_init(&s_reg, s_slots, (uint16_t)k_app_reg_cap) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_nav_init(&s_nav, &s_reg, s_nav_stack, (uint16_t)k_app_nav_cap) != k_ra8_ok) {
    return false;
  }
  s_reader_app = (ra8_app_t){.vt        = &k_app_stub_vt,
                             .ctx       = &s_reader_ctx,
                             .id        = (uint16_t)k_app_id_reader,
                             .name      = "reader",
                             .removable = false};
  bool ok      = (ra8_app_register(&s_reg, &s_reader_app) == k_ra8_ok);
#if APP_LAUNCH_SETTINGS
  s_settings_app = (ra8_app_t){.vt        = &k_app_stub_vt,
                               .ctx       = &s_settings_ctx,
                               .id        = (uint16_t)k_app_id_settings,
                               .name      = "settings",
                               .removable = true};
  if (ok) {
    ok = (ra8_app_register(&s_reg, &s_settings_app) == k_ra8_ok);
  }
#endif
  return ok;
}

/**
 * @brief Launch the `reader` app from the chrome and verify first focus.
 *
 * @details
 * The first `ra8_app_nav_go` has no prior focus, so it pushes nothing: the
 * back-stack stays empty and only `reader.on_enter` fires. Then the chrome
 * renders the foreground app once.
 *
 * @return true if reader entered exactly once with an empty back-stack.
 * @retval true  `enters == 1` and `depth == 0` after the launch + render.
 * @retval false Any nav/render call failed or a counter mismatched.
 *
 * @pre ::app_launch_register succeeded.
 * @pre No app has been launched yet (counters are zero).
 * @post On true, `reader` is foreground and rendered once.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_launch_enter_reader(void)
{
  uint16_t depth = 1U;
  if (ra8_app_nav_go(&s_nav, (uint16_t)k_app_id_reader) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_render(&s_reg) != k_ra8_ok) {
    return false;
  }
  if (s_reader_ctx.enters != 1U) {
    return false;
  }
  if (ra8_app_nav_depth(&s_nav, &depth) != k_ra8_ok) {
    return false;
  }
  return (depth == 0U);
}

/**
 * @brief Route a back-button event through the chrome to the active app.
 *
 * @details
 * Proves the "event" leg of the vtable: `ra8_app_route_input` delivers the event
 * to the foreground app's `on_input`, which counts it and consumes the back
 * button. Focus is unchanged (the stub just reports it handled the event).
 *
 * @return true if the active app consumed the event and counted it once.
 * @retval true  `out_handled` was true and `reader.inputs == 1`.
 * @retval false Routing failed, was not handled, or the counter mismatched.
 *
 * @pre `reader` is the foreground app and has not yet seen an input.
 * @pre The registry is initialised.
 * @post `reader.inputs == 1`; focus is unchanged.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_launch_route_back(void)
{
  const ra8_widget_event_t back    = {.kind      = k_ra8_widget_ev_button,
                                      .button_id = (uint16_t)k_app_btn_back};
  bool                     handled = false;
  if (ra8_app_route_input(&s_reg, &back, &handled) != k_ra8_ok) {
    return false;
  }
  if (!handled) {
    return false;
  }
  return (s_reader_ctx.inputs == 1U);
}

#if APP_LAUNCH_SETTINGS
/**
 * @brief Switch reader -> settings and back, verifying the focus + back-stack.
 *
 * @details
 * `ra8_app_nav_go(settings)` fires `reader.on_leave` then `settings.on_enter`
 * and pushes `reader` (depth 1). `ra8_app_nav_back` pops it: `settings.on_leave`
 * then `reader.on_enter` (reader's second enter), depth back to 0.
 *
 * @return true if the full switch + back sequence matched the expected counts.
 * @retval true  Lifecycle counts and the popped flag all matched.
 * @retval false Any nav call failed or a counter / depth mismatched.
 *
 * @pre ::app_launch_enter_reader succeeded (reader is foreground, depth 0).
 * @pre ::APP_LAUNCH_SETTINGS is enabled (two apps are registered).
 * @post On true, `reader` is foreground again with an empty back-stack.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_launch_switch_and_back(void)
{
  uint16_t depth  = 0U;
  bool     popped = false;
  if (ra8_app_nav_go(&s_nav, (uint16_t)k_app_id_settings) != k_ra8_ok) {
    return false;
  }
  if (s_reader_ctx.leaves != 1U) {
    return false;
  }
  if (s_settings_ctx.enters != 1U) {
    return false;
  }
  if (ra8_app_nav_depth(&s_nav, &depth) != k_ra8_ok) {
    return false;
  }
  if (depth != 1U) {
    return false;
  }
  if (ra8_app_nav_back(&s_nav, &popped) != k_ra8_ok) {
    return false;
  }
  if (!popped) {
    return false;
  }
  if (s_settings_ctx.leaves != 1U) {
    return false;
  }
  return (s_reader_ctx.enters == 2U);
}
#endif

/**
 * @brief Run the whole deterministic registry + launch + back-stack surface.
 *
 * @details
 * Composes ::app_launch_enter_reader, the optional ::app_launch_switch_and_back
 * (only when settings ships), and a final check that `reader` is the active app.
 *
 * @return true if every step held.
 * @retval true  All sub-checks passed and `reader` is active.
 * @retval false Some sub-check failed.
 *
 * @pre ::app_launch_register succeeded.
 * @pre Called once, before the idle loop.
 * @post On true, `reader` is the foreground app.
 * @post On false, `main` logs a failure and parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_launch_selfcheck(void)
{
  if (!app_launch_enter_reader()) {
    return false;
  }
  if (!app_launch_route_back()) {
    return false;
  }
#if APP_LAUNCH_SETTINGS
  if (!app_launch_switch_and_back()) {
    return false;
  }
#endif
  ra8_app_t* act = nullptr;
  if (ra8_app_active(&s_reg, &act) != k_ra8_ok) {
    return false;
  }
  if (act == nullptr) {
    return false;
  }
  return (act->id == (uint16_t)k_app_id_reader);
}

/**
 * @brief Emit the PASS banner: app count, reader enters, and the PASS line.
 *
 * @details
 * The app count proves the build-time exclusion (2 with settings, 1 without);
 * the reader-enter count proves the back-stack returned focus.
 *
 * @pre The self-check passed.
 * @pre `ra8_log_init` has run (otherwise the lines are dropped).
 * @post Three `[app_launch] INFO: ...` lines are emitted (or dropped).
 * @post No app/registry state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_launch_banner(void)
{
  uint16_t n = 0U;
  (void)ra8_app_count(&s_reg, &n);
  (void)n; /* used only by the (level-gated) log lines below */
  ra8_log_info_val("app_launch", "apps", (uint32_t)n);
  ra8_log_info_val("app_launch", "reader enters", s_reader_ctx.enters);
  ra8_log_info("app_launch", "demo PASS");
  /* Additive HIL banner: the Pi rig has no ITM/SWO capture, so mirror the PASS
   * verdict to the SCI8 / J-Link OB VCOM console for uart_scrape to gate. */
  app_launch_hil_emit_pass();
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
/**
 * @brief App entry: register the apps, run the self-check, emit PASS, park.
 *
 * @return Never returns (parks in WFI).
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success the PASS banner is emitted; otherwise a FAIL line is.
 * @post The CPU parks in WFI (observable on ra8_emulator until its budget).
 * @since 0.1.0
 */
int32_t main(void)
{
  ra8_log_init();
  ra8_log_info("app_launch", "boot");

  /* Bring up the VCOM console so the success path can emit the HIL banner.
   * Best-effort: a failure is narrated over ITM and the demo continues. */
  if (!app_launch_hil_console_init()) {
    ra8_log_info("app_launch", "VCOM console init failed -- HIL banner unavailable");
  }

  bool ok = app_launch_register();
  if (!ok) {
    ra8_log_info("app_launch", "FAIL register");
  }
  if (ok) {
    ok = app_launch_selfcheck();
    if (!ok) {
      ra8_log_info("app_launch", "FAIL selfcheck");
    }
  }
  if (ok) {
    app_launch_banner();
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
#pragma GCC diagnostic pop
