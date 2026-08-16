/**
 * @file examples/ek_ra8d2/hw_validated/hil/app_shell_demo/main.c
 * @brief App-shell chrome: launch reader / library / settings apps (#146 Ph2).
 *
 * @details
 * The chrome / "shell" increment of the app framework (issue #146, Phase 2). It
 * builds on the Phase-1 `ra8_app` registry + per-app vtable + navigation
 * back-stack (`app_launch_demo`) and adds the piece a home screen needs: a small
 * launcher that lists the registered apps and launches the one the user picks,
 * with "back" unwinding the navigation trail. No display and no widgets this
 * increment -- the path is observable headlessly on `ra8_emulator` through the ITM
 * log (each line shows up as `[itm] ...`).
 *
 * What it does, acting as the device "chrome":
 *   1. Registers three first-class apps into one registry, each a real `ra8_app`
 *      vtable (init / on_enter / render(draw) / on_input(event) / on_leave /
 *      deinit(teardown)):
 *      - `library`  (id 1) -- **core, non-removable** (`removable = false`): the
 *        file/book organizer.
 *      - `reader`   (id 2) -- **core, non-removable** (`removable = false`): the
 *        EPUB reader.
 *      - `settings` (id 3) -- **optional, removable** (`removable = true`),
 *        wrapped in a build-time guard (`#if APP_SHELL_SETTINGS`). Building with
 *        `-DAPP_SHELL_SETTINGS=0` drops it from the registry entirely -- the
 *        "core uninstallable" build-time-exclusion mechanism from #146 -- and the
 *        shell still builds + runs (it just skips the settings leg).
 *   2. Presents a launcher: enumerates the registry (`ra8_app_count` +
 *      `ra8_app_at`), logs each app as a menu entry, and launches one **by its
 *      position** through `ra8_app_nav_go_index` (the by-index launcher bridge).
 *   3. Navigates library -> reader -> settings with `ra8_app_nav_go`, the focus
 *      lifecycle firing under every move (`on_leave` -> `on_enter`) and each
 *      outgoing app pushed onto the back-stack.
 *   4. Presses "back" twice with `ra8_app_nav_back`, unwinding settings -> reader
 *      -> library so the back-stack empties and `library` is foreground again.
 *
 * Each stub app's callbacks just count and log their lifecycle (no real UI). A
 * deterministic self-check asserts the exact call counts + back-stack depth at
 * every step, then emits:
 *
 *   `[app_shell] INFO: app_shell_demo PASS`
 *   (ra8_emulator: `[itm] [app_shell] INFO: app_shell_demo PASS`)
 *
 * so the app doubles as a `ra8_emulator` regression gate; any failure logs a
 * `FAIL ...` line and parks in WFI.
 *
 * After the navigation legs the shell exercises the run-time "core uninstallable"
 * rule: it asks the framework to uninstall the **core** `library` app (refused,
 * `k_ra8_err_not_supported`) and then the **removable** `settings` app (unmounted
 * -- its `deinit` fires and it leaves the registry), proving the `removable` flag
 * has teeth at run time and not just at build time.
 *
 * @note Wiring real `ra8_widget` UIs into each app (an "app = a widget tree") is
 *       the next increment. It is deliberately kept out here so this shell stays
 *       decoupled from `ra8_widget`'s in-flight changes: the framework only routes
 *       the lifecycle + input + render to the active app, and the concrete draw
 *       lives in each app's `render` callback (a no-op-but-logged stub today).
 * @note Each app's `deinit` callback is exercised by the uninstall leg below
 *       (`ra8_app_uninstall` runs an unmounted app's `deinit` once); a core app's
 *       `deinit` never fires because a core app can never be uninstalled.
 *
 *
 * [Ring 6 / APP] {World: S}
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_app.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_err.h"
#include "ra8_log.h"
#include "ra8_widget.h"

/**
 * @enum app_shell_hil_t
 * @brief VCOM-console line rate for the deterministic HIL success banner.
 * @details The EK-RA8D2 J-Link OB VCOM bridge (SCI8, PD02/PD03) runs 8N1 at this
 *          rate; the Pi HIL rig's `uart_scrape` reads /dev/ttyACM0 to gate the
 *          app. The banner is additive to the existing `ra8_log` ITM trace.
 * @since 0.1.0
 */
typedef enum : uint32_t {
  k_app_shell_hil_baud = 115200U, /**< VCOM console line rate (8N1). */
} app_shell_hil_t;

/**
 * @var k_app_shell_pass_banner
 * @brief Deterministic, run-to-run-stable HIL success banner (uart_scrape gate).
 * @details Emitted over the SCI8 / J-Link OB VCOM console only on the success
 *          path, AFTER the launcher + navigation self-check passes. The ITM
 *          `ra8_log` verdict is left intact; this is purely additive so the Pi
 *          rig (which has no SWO / ITM capture) can gate the app.
 * @note Trailing CRLF terminates the line on the wire; the gate matches the text.
 * @warning Do not modify; the HIL gate (hil.conf HIL_EXPECT) matches it exactly.
 * @since 0.1.0
 */
static const uint8_t k_app_shell_pass_banner[] = "app_shell_demo: demo PASS\r\n";

/**
 * @brief Bring up the SCI8 / J-Link OB VCOM console for the HIL success banner.
 *
 * @details
 * Configures the clock tree (`ra8_cgc_init`, which publishes the PCLKA the SCI8
 * BRR divisor is computed from) then the EK-RA8D2 debug console
 * (`ra8_board_uart_console_init`, SCI8 on PD02/PD03 at ::k_app_shell_hil_baud).
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
static bool app_shell_hil_console_init(void)
{
  if (ra8_cgc_init() != k_ra8_ok) {
    return false;
  }
  if (ra8_board_uart_console_init((uint32_t)k_app_shell_hil_baud) != k_ra8_ok) {
    return false;
  }
  return true;
}

/**
 * @brief Emit the deterministic HIL success banner over the VCOM console.
 *
 * @details
 * Writes ::k_app_shell_pass_banner to the SCI8 / J-Link OB VCOM console and
 * flushes it so the bytes clock out before the CPU parks in WFI. A no-op if the
 * console never came up (the write returns `k_ra8_err_not_initialized`, ignored).
 *
 * @return Nothing.
 *
 * @pre Reached only on the verified success path (single, non-compound guard).
 * @pre ::app_shell_hil_console_init was attempted during bring-up.
 * @post The banner has been handed to SCI8 and the TX FIFO drained (if up).
 * @post No registry / app state is modified.
 *
 * @note Not thread-safe; single-threaded init context.
 * @since 0.1.0
 */
static void app_shell_hil_emit_pass(void)
{
  (void)ra8_board_uart_console_write(k_app_shell_pass_banner,
                                     (size_t)(sizeof(k_app_shell_pass_banner) - 1U));
  (void)ra8_board_uart_console_flush();
}

/**
 * @def APP_SHELL_SETTINGS
 * @brief Build-time selector for the optional `settings` app (#146 exclusion).
 *
 * @details
 * Defaults to 1 (settings shipped). Define it to 0 at configure time
 * (`-DAPP_SHELL_SETTINGS=0`) to exclude the removable `settings` app from the
 * registry, demonstrating the "core uninstallable" build-time-exclusion path:
 * the shell still builds and runs, with `settings` simply absent and the
 * settings navigation leg skipped.
 *
 * @note Compile-time only; no run-time cost.
 * @since 0.1.0
 */
#ifndef APP_SHELL_SETTINGS
/** @brief APP SHELL SETTINGS. */
#define APP_SHELL_SETTINGS (1)
#endif

/**
 * @enum app_shell_id_t
 * @brief Registered app ids (launch keys).
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_app_id_library  = 1U, /**< Core file/book organizer (non-removable). */
  k_app_id_reader   = 2U, /**< Core EPUB-reader app (non-removable).     */
  k_app_id_settings = 3U, /**< Optional settings app (removable).        */
} app_shell_id_t;

/**
 * @enum app_shell_const_t
 * @brief Capacities, the launcher tile index, and the chrome button id.
 * @since 0.1.0
 */
typedef enum : uint16_t {
  k_app_reg_cap     = 3U, /**< Registry storage slots (library/reader/settings). */
  k_app_nav_cap     = 4U, /**< Back-stack depth (history of focused apps).       */
  k_app_idx_library = 0U, /**< Registry index of `library` (first registered).   */
  k_app_btn_back    = 1U, /**< Button id a chrome routes to mean "go back".      */
} app_shell_const_t;

/**
 * @struct app_shell_ctx_t
 * @brief Per-app stub state: a display name + lifecycle-call counters.
 *
 * @details
 * Each registered app points its `ctx` at one of these. The vtable callbacks
 * bump the matching counter and log the event, so the self-check can assert the
 * exact focus lifecycle (how many inits/enters/leaves/renders/inputs fired)
 * without any display.
 *
 * @invariant `enters >= leaves` while the app is foreground.
 * @since 0.1.0
 */
typedef struct {
  const char* name;    /**< Display name + ITM log tag (non-NULL). */
  uint32_t    inits;   /**< Times `init` fired (at register).      */
  uint32_t    enters;  /**< Times `on_enter` fired.                */
  uint32_t    leaves;  /**< Times `on_leave` fired.                */
  uint32_t    renders; /**< Times `render` fired.                  */
  uint32_t    inputs;  /**< Times `on_input` fired.                */
  uint32_t    deinits; /**< Times `deinit` fired (teardown).       */
} app_shell_ctx_t;

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
 * @var s_library_ctx
 * @brief Stub state for the core `library` app.
 * @note Read/written by the library app's vtable callbacks only.
 * @warning Counters are asserted by the self-check; do not reset elsewhere.
 * @since 0.1.0
 */
static app_shell_ctx_t s_library_ctx = {.name = "library"};

/**
 * @var s_library_app
 * @brief The core `library` app instance (registered into ::s_reg).
 * @note Built in `app_shell_register`.
 * @warning Must outlive the registry (static storage).
 * @since 0.1.0
 */
static ra8_app_t s_library_app;

/**
 * @var s_reader_ctx
 * @brief Stub state for the core `reader` app.
 * @note Read/written by the reader app's vtable callbacks only.
 * @warning Counters are asserted by the self-check; do not reset elsewhere.
 * @since 0.1.0
 */
static app_shell_ctx_t s_reader_ctx = {.name = "reader"};

/**
 * @var s_reader_app
 * @brief The core `reader` app instance (registered into ::s_reg).
 * @note Built in `app_shell_register`.
 * @warning Must outlive the registry (static storage).
 * @since 0.1.0
 */
static ra8_app_t s_reader_app;

#if APP_SHELL_SETTINGS
/**
 * @var s_settings_ctx
 * @brief Stub state for the optional `settings` app.
 * @note Compiled only when ::APP_SHELL_SETTINGS is enabled.
 * @warning Counters are asserted by the self-check.
 * @since 0.1.0
 */
static app_shell_ctx_t s_settings_ctx = {.name = "settings"};

/**
 * @var s_settings_app
 * @brief The optional `settings` app instance (registered into ::s_reg).
 * @note Compiled only when ::APP_SHELL_SETTINGS is enabled.
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
 * @brief Stub `init`: count the one-time setup and log it (runs at register).
 * @param[in,out] a The app being registered (non-NULL, `ctx` is app_shell_ctx_t).
 * @return k_ra8_ok always (the stub never fails init).
 * @retval k_ra8_ok Setup recorded.
 * @pre `a` and `a->ctx` are non-NULL.
 * @pre `ra8_log_init` has run.
 * @post The app's `inits` counter advanced by one.
 * @post One `[name] INFO: init` line is emitted (or dropped if no ITM).
 * @note Not thread-safe; called from `ra8_app_register`.
 * @since 0.1.0
 */
static ra8_err_t app_stub_init(ra8_app_t* a)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
  c->inits++;
  ra8_log_info(c->name, "init");
  return k_ra8_ok;
}

/**
 * @brief Stub `on_enter`: count the focus gain and log it.
 * @param[in,out] a The app gaining focus (non-NULL, `ctx` is app_shell_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @pre `ra8_log_init` has run (otherwise the log line is dropped).
 * @post The app's `enters` counter advanced by one.
 * @post One `[name] INFO: on_enter` line is emitted (or dropped).
 * @note Not thread-safe; called from the foreground switch path.
 * @since 0.1.0
 */
static void app_stub_on_enter(ra8_app_t* a)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
  c->enters++;
  ra8_log_info(c->name, "on_enter");
}

/**
 * @brief Stub `on_leave`: count the focus loss and log it.
 * @param[in,out] a The app losing focus (non-NULL, `ctx` is app_shell_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @pre `ra8_log_init` has run (otherwise the log line is dropped).
 * @post The app's `leaves` counter advanced by one.
 * @post One `[name] INFO: on_leave` line is emitted (or dropped).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_stub_on_leave(ra8_app_t* a)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
  c->leaves++;
  ra8_log_info(c->name, "on_leave");
}

/**
 * @brief Stub `render`: count the frame and log it (stands in for drawing).
 * @param[in] a The foreground app (non-NULL, `ctx` is app_shell_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @pre `ra8_log_init` has run (otherwise the log line is dropped).
 * @post The app's `renders` counter advanced by one.
 * @post One `[name] INFO: render` line is emitted (or dropped).
 * @note Not thread-safe; the real on-target draw (an `ra8_widget` tree) lives
 *       here in a later increment.
 * @since 0.1.0
 */
static void app_stub_render(const ra8_app_t* a)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
  c->renders++;
  ra8_log_info(c->name, "render");
}

/**
 * @brief Stub `on_input`: count the event, consume only the "back" button.
 * @param[in,out] a  The foreground app (non-NULL, `ctx` is app_shell_ctx_t).
 * @param[in]     ev The input event (non-NULL).
 * @return true if the event was the back button (consumed), false otherwise.
 * @retval true  `ev` is a ::k_app_btn_back button press.
 * @retval false Any other event (the chrome keeps routing it).
 * @pre `a`, `a->ctx`, and `ev` are non-NULL.
 * @pre `ra8_log_init` has run (otherwise the log line is dropped).
 * @post The app's `inputs` counter advanced by one.
 * @post On a back press one `[name] INFO: back requested` line is emitted.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_stub_on_input(ra8_app_t* a, const ra8_widget_event_t* ev)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
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
 * @brief Stub `deinit`: count the teardown and log it (wired, not yet fired).
 * @param[in,out] a The app being torn down (non-NULL, `ctx` is app_shell_ctx_t).
 * @pre `a` and `a->ctx` are non-NULL.
 * @pre `ra8_log_init` has run (otherwise the log line is dropped).
 * @post The app's `deinits` counter advanced by one.
 * @post One `[name] INFO: deinit` line is emitted (or dropped).
 * @note Not thread-safe. The framework has no unregister trigger yet, so this is
 *       present for contract completeness but not invoked this increment.
 * @since 0.1.0
 */
static void app_stub_deinit(ra8_app_t* a)
{
  app_shell_ctx_t* c = (app_shell_ctx_t*)a->ctx;
  c->deinits++;
  ra8_log_info(c->name, "deinit");
}

/**
 * @var k_app_stub_vt
 * @brief Shared lifecycle vtable for every stub app (behaviour keyed off ctx).
 * @details Every callback except `tick` is wired (init / on_enter / render /
 *          on_input / on_leave / deinit); `tick` stays NULL to prove the
 *          framework tolerates a partial vtable.
 * @note Read-only; lives in .rodata.
 * @since 0.1.0
 */
static const ra8_app_vtable_t k_app_stub_vt = {
  .init     = app_stub_init,
  .on_enter = app_stub_on_enter,
  .tick     = nullptr,
  .render   = app_stub_render,
  .on_input = app_stub_on_input,
  .on_leave = app_stub_on_leave,
  .deinit   = app_stub_deinit,
};

/* ===========================================================================
 * Registration + launcher + self-check (the "chrome")
 * ===========================================================================
 */

/**
 * @brief Build one app instance from the shared vtable and register it.
 *
 * @details
 * Fills @p app with the shared ::k_app_stub_vt, the supplied @p ctx / @p id /
 * @p name / @p removable flag, and registers it into ::s_reg (which runs its
 * `init`). Factored out so ::app_shell_register stays short.
 *
 * @param[out]    app       App instance storage to populate (non-NULL).
 * @param[in,out] ctx       Per-app stub state the vtable keys off (non-NULL).
 * @param[in]     id        Unique launch id.
 * @param[in]     name      Display name + log tag (non-NULL, static lifetime).
 * @param[in]     removable Optional/uninstallable flag (false = core).
 *
 * @return true if `ra8_app_register` returned k_ra8_ok.
 * @retval true  Registered + initialised.
 * @retval false Registration failed (duplicate id, full, or init error).
 *
 * @pre ::s_reg is initialised.
 * @pre @p app, @p ctx, and @p name are non-NULL.
 * @post On true ::s_reg holds @p app and its `init` ran once.
 * @post On false ::s_reg is unchanged.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool
app_shell_add(ra8_app_t* app, app_shell_ctx_t* ctx, uint16_t id, const char* name, bool removable)
{
  *app =
    (ra8_app_t){.vt = &k_app_stub_vt, .ctx = ctx, .id = id, .name = name, .removable = removable};
  return (ra8_app_register(&s_reg, app) == k_ra8_ok);
}

/**
 * @brief Initialise the registry + back-stack and register every app.
 *
 * @details
 * Binds ::s_reg and ::s_nav to their caller-owned storage, then registers the
 * core `library` and `reader` apps (non-removable) and -- when
 * ::APP_SHELL_SETTINGS is on -- the optional `settings` app (removable). Any
 * failure short-circuits to a false return so `main` can log and park.
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
static bool app_shell_register(void)
{
  if (ra8_app_registry_init(&s_reg, s_slots, (uint16_t)k_app_reg_cap) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_nav_init(&s_nav, &s_reg, s_nav_stack, (uint16_t)k_app_nav_cap) != k_ra8_ok) {
    return false;
  }
  bool ok =
    app_shell_add(&s_library_app, &s_library_ctx, (uint16_t)k_app_id_library, "library", false);
  if (ok) {
    ok = app_shell_add(&s_reader_app, &s_reader_ctx, (uint16_t)k_app_id_reader, "reader", false);
  }
#if APP_SHELL_SETTINGS
  if (ok) {
    ok = app_shell_add(&s_settings_app,
                       &s_settings_ctx,
                       (uint16_t)k_app_id_settings,
                       "settings",
                       true);
  }
#endif
  return ok;
}

/**
 * @brief Log the launcher "menu": every registered app, its id, and its class.
 *
 * @details
 * Enumerates the registry the way a home screen would (`ra8_app_count` +
 * `ra8_app_at`) and logs each app as `[name] INFO: core|removable=<id>`. This is
 * the launcher's list view (text stand-in for the on-screen tiles). The loop is
 * bounded by the registry capacity (::k_app_reg_cap), so it is statically
 * provable (NASA Rule 2).
 *
 * @pre ::app_shell_register succeeded.
 * @pre `ra8_log_init` has run (otherwise the lines are dropped).
 * @post One menu line is emitted per registered app (or all dropped if no ITM).
 * @post No app/registry state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_shell_log_menu(void)
{
  uint16_t n = 0U;
  (void)ra8_app_count(&s_reg, &n);
  RA8_LOOP_BOUND(k_app_reg_cap);
  for (uint16_t i = 0U; i < n; i++) {
    ra8_app_t* app = nullptr;
    if (ra8_app_at(&s_reg, i, &app) == k_ra8_ok) {
      if (app != nullptr) {
        ra8_log_info_val(app->name, app->removable ? "removable" : "core", (uint32_t)app->id);
      }
    }
  }
}

/**
 * @brief Launcher: list the apps then launch `library` by its registry index.
 *
 * @details
 * Shows the menu, then launches the app at ::k_app_idx_library through
 * `ra8_app_nav_go_index` -- the by-position launcher bridge. The first launch has
 * no prior focus, so it pushes nothing: the back-stack stays empty and only
 * `library.on_enter` fires. The chrome then renders the foreground app once.
 *
 * @return true if `library` entered exactly once with an empty back-stack.
 * @retval true  `enters == 1` and `depth == 0` after the launch + render.
 * @retval false Any launcher/nav/render call failed or a counter mismatched.
 *
 * @pre ::app_shell_register succeeded.
 * @pre No app has been launched yet (counters are zero).
 * @post On true, `library` is foreground and rendered once.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_launch_library(void)
{
  uint16_t depth = 1U;
  app_shell_log_menu();
  if (ra8_app_nav_go_index(&s_nav, (uint16_t)k_app_idx_library) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_render(&s_reg) != k_ra8_ok) {
    return false;
  }
  if (s_library_ctx.enters != 1U) {
    return false;
  }
  if (ra8_app_nav_depth(&s_nav, &depth) != k_ra8_ok) {
    return false;
  }
  return (depth == 0U);
}

/**
 * @brief Open `reader` from `library`, then route a back-button event to it.
 *
 * @details
 * `ra8_app_nav_go(reader)` fires `library.on_leave` then `reader.on_enter` and
 * pushes `library` (depth 1); the chrome renders the new foreground app. Then a
 * back-button event is routed through `ra8_app_route_input` to prove the event
 * leg of the vtable: `reader.on_input` counts and consumes it (focus unchanged).
 *
 * @return true if the open + render + input round-trip matched the expected
 *         counts and depth.
 * @retval true  `library.leaves == 1`, `reader.enters == 1`, `reader.inputs == 1`,
 *               the event was handled, and depth == 1.
 * @retval false Any nav/render/route call failed or a counter mismatched.
 *
 * @pre ::app_shell_launch_library succeeded (`library` foreground, depth 0).
 * @pre `reader` and `library` are both registered (always true: core apps).
 * @post On true, `reader` is foreground with `library` on the back-stack.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_open_reader(void)
{
  uint16_t depth = 0U;
  if (ra8_app_nav_go(&s_nav, (uint16_t)k_app_id_reader) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_render(&s_reg) != k_ra8_ok) {
    return false;
  }
  if (s_library_ctx.leaves != 1U) {
    return false;
  }
  if (s_reader_ctx.enters != 1U) {
    return false;
  }
  const ra8_widget_event_t back    = {.kind      = k_ra8_widget_ev_button,
                                      .button_id = (uint16_t)k_app_btn_back};
  bool                     handled = false;
  if (ra8_app_route_input(&s_reg, &back, &handled) != k_ra8_ok) {
    return false;
  }
  if (!handled) {
    return false;
  }
  if (s_reader_ctx.inputs != 1U) {
    return false;
  }
  if (ra8_app_nav_depth(&s_nav, &depth) != k_ra8_ok) {
    return false;
  }
  return (depth == 1U);
}

#if APP_SHELL_SETTINGS
/**
 * @brief Open `settings` from `reader`, then go "back" to `reader`.
 *
 * @details
 * `ra8_app_nav_go(settings)` fires `reader.on_leave` then `settings.on_enter` and
 * pushes `reader` (depth 2); the chrome renders it. `ra8_app_nav_back` pops it:
 * `settings.on_leave` then `reader.on_enter` (reader's second enter), depth back
 * to 1.
 *
 * @return true if the open + back round-trip matched the expected counts.
 * @retval true  Lifecycle counts, the popped flag, and depth all matched.
 * @retval false Any nav/render call failed or a counter / depth mismatched.
 *
 * @pre ::app_shell_open_reader succeeded (`reader` foreground, depth 1).
 * @pre ::APP_SHELL_SETTINGS is enabled (the settings app is registered).
 * @post On true, `reader` is foreground again with `library` still on the stack.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_settings_round_trip(void)
{
  uint16_t depth  = 0U;
  bool     popped = false;
  if (ra8_app_nav_go(&s_nav, (uint16_t)k_app_id_settings) != k_ra8_ok) {
    return false;
  }
  if (ra8_app_render(&s_reg) != k_ra8_ok) {
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
  if (depth != 2U) {
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
 * @brief Press "back" to the root: pop to `library` and confirm the empty trail.
 *
 * @details
 * `ra8_app_nav_back` pops the last remembered app (`library`): the foreground app
 * leaves, `library.on_enter` fires (its second enter), and the back-stack
 * empties (depth 0). Works in both builds: the foreground app is `reader` (no
 * settings) or `reader` again after the settings round-trip.
 *
 * @return true if `library` re-entered with an empty back-stack.
 * @retval true  `library.enters == 2`, the app was popped, and depth == 0.
 * @retval false The nav call failed, nothing popped, or a counter mismatched.
 *
 * @pre `library` is the only app remaining on the back-stack (depth 1).
 * @pre The previous navigation legs all succeeded (caller short-circuits else).
 * @post On true, `library` is foreground and the trail is empty.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_back_to_root(void)
{
  uint16_t depth =
    9U; /* MAGIC-OK: expected back-stack depth at this point in the launch/switch self-check */
  bool popped = false;
  if (ra8_app_nav_back(&s_nav, &popped) != k_ra8_ok) {
    return false;
  }
  if (!popped) {
    return false;
  }
  if (s_library_ctx.enters != 2U) {
    return false;
  }
  if (ra8_app_nav_depth(&s_nav, &depth) != k_ra8_ok) {
    return false;
  }
  return (depth == 0U);
}

#if APP_SHELL_SETTINGS
/**
 * @brief Exercise the run-time "core uninstallable" rule: refuse core, unmount one.
 *
 * @details
 * With `library` foreground and `settings` registered-but-background, asks the
 * framework to uninstall the **core** `library` app -- refused with
 * `k_ra8_err_not_supported`, nothing torn down -- and then the **removable**
 * `settings` app, which unmounts: its `deinit` fires once, it leaves the registry
 * (count drops to two), and ::ra8_app_state reports it as unmounted. `library`
 * stays foreground (its slot precedes the removed one, so its active index is
 * untouched).
 *
 * @return true if the core uninstall was refused and settings unmounted cleanly.
 * @retval true  Core refused, settings deinit fired once, count == 2, settings
 *               reports unmounted.
 * @retval false Any uninstall returned an unexpected code or a count/state
 *               mismatched.
 *
 * @pre ::app_shell_back_to_root succeeded (`library` foreground, depth 0).
 * @pre ::APP_SHELL_SETTINGS is enabled (the settings app is registered).
 * @post On true, only `library` + `reader` remain registered and `library` is
 *       still the foreground app.
 * @post On false, the self-check aborts and `main` parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_uninstall_demo(void)
{
  /* Core app: uninstall must be refused and tear nothing down. */
  if (ra8_app_uninstall(&s_reg, (uint16_t)k_app_id_library) != k_ra8_err_not_supported) {
    return false;
  }
  if (s_library_ctx.deinits != 0U) {
    return false;
  }
  /* Removable, background app: uninstall unmounts it (deinit fires). */
  if (ra8_app_uninstall(&s_reg, (uint16_t)k_app_id_settings) != k_ra8_ok) {
    return false;
  }
  if (s_settings_ctx.deinits != 1U) {
    return false;
  }
  ra8_app_state_t st = k_ra8_app_state_foreground;
  if (ra8_app_state(&s_reg, (uint16_t)k_app_id_settings, &st) != k_ra8_ok) {
    return false;
  }
  if (st != k_ra8_app_state_unmounted) {
    return false;
  }
  uint16_t n = 0U;
  if (ra8_app_count(&s_reg, &n) != k_ra8_ok) {
    return false;
  }
  return (n == 2U); /* library + reader remain */
}
#endif

/**
 * @brief Run the whole deterministic launcher + navigate + back-stack surface.
 *
 * @details
 * Composes ::app_shell_launch_library, ::app_shell_open_reader, the optional
 * ::app_shell_settings_round_trip (only when settings ships),
 * ::app_shell_back_to_root, the optional ::app_shell_uninstall_demo (the run-time
 * core-uninstallable rule, only when settings ships), and a final check that
 * `library` is the active app.
 *
 * @return true if every step held.
 * @retval true  All sub-checks passed and `library` is active.
 * @retval false Some sub-check failed.
 *
 * @pre ::app_shell_register succeeded.
 * @pre Called once, before the idle loop.
 * @post On true, `library` is the foreground app.
 * @post On false, `main` logs a failure and parks.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool app_shell_selfcheck(void)
{
  if (!app_shell_launch_library()) {
    return false;
  }
  if (!app_shell_open_reader()) {
    return false;
  }
#if APP_SHELL_SETTINGS
  if (!app_shell_settings_round_trip()) {
    return false;
  }
#endif
  if (!app_shell_back_to_root()) {
    return false;
  }
#if APP_SHELL_SETTINGS
  if (!app_shell_uninstall_demo()) {
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
  return (act->id == (uint16_t)k_app_id_library);
}

/**
 * @brief Emit the PASS banner: app count, library enters, and the PASS line.
 *
 * @details
 * The app count is two in both builds, but for different reasons: without
 * settings only `library` + `reader` ever register (build-time exclusion); with
 * settings all three register and the run-time uninstall leg then unmounts
 * `settings`, leaving the same two. The library-enter count proves the
 * back-stack returned focus to the root.
 *
 * @pre The self-check passed.
 * @pre `ra8_log_init` has run (otherwise the lines are dropped).
 * @post Three `[app_shell] INFO: ...` lines are emitted (or dropped).
 * @post No app/registry state is modified.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void app_shell_banner(void)
{
  uint16_t n = 0U;
  (void)ra8_app_count(&s_reg, &n);
  ra8_log_info_val("app_shell", "apps", (uint32_t)n);
  ra8_log_info_val("app_shell", "library enters", s_library_ctx.enters);
  ra8_log_info("app_shell", "app_shell_demo PASS");
  /* Additive HIL banner: the Pi rig has no ITM/SWO capture, so mirror the PASS
   * verdict to the SCI8 / J-Link OB VCOM console for uart_scrape to gate. */
  app_shell_hil_emit_pass();
}

/**
 * @brief App entry: register the apps, run the launcher self-check, emit PASS.
 *
 * @pre Reset_Handler copied .data and zeroed .bss.
 * @pre SystemInit set VTOR / FPU / priority grouping.
 * @post On success the PASS banner is emitted; otherwise a FAIL line is.
 * @post The CPU parks in WFI (observable on ra8_emulator until its budget).
 * @since 0.1.0
 */
void main(void)
{
  ra8_log_init();
  ra8_log_info("app_shell", "boot");

  /* Bring up the VCOM console so the success path can emit the HIL banner.
   * Best-effort: a failure is narrated over ITM and the demo continues. */
  if (!app_shell_hil_console_init()) {
    ra8_log_info("app_shell", "VCOM console init failed -- HIL banner unavailable");
  }

  bool ok = app_shell_register();
  if (!ok) {
    ra8_log_info("app_shell", "FAIL register");
  }
  if (ok) {
    ok = app_shell_selfcheck();
    if (!ok) {
      ra8_log_info("app_shell", "FAIL selfcheck");
    }
  }
  if (ok) {
    app_shell_banner();
  }

  while (1) {
    __asm__ volatile("wfi");
  }
}
