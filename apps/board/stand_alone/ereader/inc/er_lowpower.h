/**
 * @file er_lowpower.h
 * @brief E-reader low-power controller and M85/M33 shared SRAM3 mailbox
 * @ingroup grp_apps
 *
 * @par Tag
 * [App / Product] {World: NS}
 *
 * @details
 * Defines the cross-core SRAM3 shared mailbox contract (`0x22180000` physical /
 * `0x32180000` Non-Secure alias) and the Non-Secure M85 hold-page policy
 * manager for `apps/board/stand_alone/ereader`.
 *
 * When enabled (default ON, toggleable at runtime via long `SW2` hold or
 * ``er_lowpower_toggle``), idle periods after page rendering hand off display
 * supervision to the secondary Cortex-M33 core (CPU1) and park the primary
 * Cortex-M85 core (CPU0) with HOCO stopped in Sleep `WFI`. When the M33
 * detects `SW1` (next page), `SW2` (toggle low-power off), or a programmed
 * `auto_turn_polls` soak event, it increments `turn_req`, signals `IPC0`, and
 * M85 wakes cleanly to render the next page via `er_panel` and re-park.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/** @brief Fixed SRAM3 mailbox addresses and protocol signatures. */
typedef enum : uint32_t {
  k_er_lp_mailbox_paddr   = 0x22180000UL, /**< Physical SRAM3 base (M33 NS / Secure).    */
  k_er_lp_mailbox_ns_addr = 0x32180000UL, /**< IDAU-NS alias seen by CPU0 Non-Secure.    */
  k_er_lp_cpu1_mram_base  = 0x020C0000UL, /**< CPU1 MRAM image origin.                   */
  k_er_lp_cpu1_stack_top  = 0x221A0000UL, /**< CPU1 initial SRAM stack top.              */
  k_er_lp_mailbox_magic   = 0x38444C50UL, /**< `"8DLP"` stamped by M85 before CPU1 boot. */
  k_er_lp_m33_ready_sig   = 0x4D333331UL, /**< `"M331"` stamped by CPU1 when running.    */
} er_lp_const_t;

/** @brief Current operational status published in shared SRAM3 mailbox. */
typedef enum : uint32_t {
  k_er_lp_status_booting  = 0U, /**< M85 released M33 and awaits ready signature. */
  k_er_lp_status_holding  = 1U, /**< M33 holds static page while M85 is parked.   */
  k_er_lp_status_turning  = 2U, /**< M85 awake rendering page turn or UI update.  */
  k_er_lp_status_disabled = 3U, /**< Low-power mode toggled off; M85 stays awake. */
} er_lp_mbox_status_t;

/** @brief Reason M33 requested an IPC0 wakeup for M85. */
typedef enum : uint32_t {
  k_er_lp_wake_none      = 0U, /**< No wake pending (timeout or spurious IRQ).   */
  k_er_lp_wake_next_page = 1U, /**< SW1 edge detected while M33 held the page.   */
  k_er_lp_wake_toggle    = 2U, /**< SW2 edge detected while holding: disable LP. */
  k_er_lp_wake_auto_test = 3U, /**< Bounded `auto_turn_polls` periodic wakeup.   */
} er_lp_wake_reason_t;

/** @brief Host-visible controller state for M85 low-power coordination. */
typedef enum : uint8_t {
  k_er_lp_state_uninit   = 0U, /**< Not initialised.                           */
  k_er_lp_state_active   = 1U, /**< Enabled and M85 actively rendering/idle.   */
  k_er_lp_state_holding  = 2U, /**< M85 entering/inside Sleep while M33 holds. */
  k_er_lp_state_disabled = 3U, /**< Low-power toggle OFF; M85 never sleeps.    */
} er_lp_state_t;

/** @brief Action emitted by ``er_lowpower_tick_inputs`` each UI tick. */
typedef enum : uint8_t {
  k_er_lp_action_none       = 0U, /**< No special low-power transition this tick. */
  k_er_lp_action_sw2_short  = 1U, /**< Short SW2 press released: cycle refresh.   */
  k_er_lp_action_toggled    = 2U, /**< Long SW2 hold toggled LP enabled state.    */
  k_er_lp_action_enter_hold = 3U, /**< Ready to park M85 and hand off to M33.     */
} er_lp_input_action_t;

/**
 * @struct er_lp_mailbox_t
 * @brief Cross-core shared SRAM3 mailbox (`0x22180000` / NS `0x32180000`).
 *
 * @details Uses fixed-width `uint32_t` fields so the layout is identical across
 * Cortex-M85, Cortex-M33, and 64-bit host test builds.
 */
typedef struct {
  uint32_t magic;           /**< Must equal `k_er_lp_mailbox_magic`.            */
  uint32_t m33_sig;         /**< Stamped `k_er_lp_m33_ready_sig` by M33.        */
  uint32_t status;          /**< Active ::er_lp_mbox_status_t state.            */
  uint32_t fb_base;         /**< SDRAM framebuffer address displayed by GLCDC.  */
  uint32_t fb_crc;          /**< Caller-provided page/framebuffer fingerprint.  */
  uint32_t held_page;       /**< Currently displayed e-reader page index.       */
  uint32_t turn_req;        /**< Monotonic wake counter bumped by M33.          */
  uint32_t turn_ack;        /**< Matched to `turn_req` by M85 after wake/draw.  */
  uint32_t wake_reason;     /**< ::er_lp_wake_reason_t written before IPC0 irq. */
  uint32_t m33_heartbeat;   /**< Incremented by M33 during active/hold polls.   */
  uint32_t m33_loops;       /**< Total outer hold-loop iterations run by M33.   */
  uint32_t auto_turn_polls; /**< Nonzero: M33 triggers wake every N hold polls. */
} er_lp_mailbox_t;

/**
 * @struct er_lp_ops_t
 * @brief Dependency-injected NSC and SysTick/WDT seam for M85 low-power handoff.
 */
typedef struct {
  ra8_err_t (*start_m33)(uintptr_t cpu1_mram_base, uintptr_t cpu1_stack_top);
  ra8_err_t (*park_m85)(uint32_t max_wake_iters, uint32_t* out_woken);
  ra8_err_t (*notify_cpu1)(void);
  void (*before_park)(void);
  void (*after_wake)(void);
} er_lp_ops_t;

/**
 * @struct er_lp_config_t
 * @brief Configuration descriptor passed to ``er_lowpower_init``.
 */
typedef struct {
  volatile er_lp_mailbox_t* mailbox;            /**< Shared SRAM3 mailbox pointer.   */
  er_lp_ops_t               ops;                /**< Platform NSC/SysTick callbacks. */
  uint32_t                  idle_threshold_ms;  /**< Idle ms before parking M85.     */
  uint32_t                  sw2_toggle_hold_ms; /**< SW2 hold ms to toggle LP mode.  */
  uint32_t                  max_park_iters;     /**< Sleep WFI bound passed to park. */
  uint32_t                  auto_turn_polls;    /**< 0=buttons only, >0 soak period. */
  uint32_t                  initial_fb_base;    /**< Initial framebuffer base addr.  */
  bool                      start_enabled;      /**< Initial low-power toggle state. */
} er_lp_config_t;

/**
 * @struct er_lp_ctx_t
 * @brief Caller-owned state for the e-reader low-power coordinator.
 */
typedef struct {
  volatile er_lp_mailbox_t* mailbox;            /**< Shared mailbox reference.       */
  er_lp_ops_t               ops;                /**< Injected NSC/park callbacks.    */
  uint32_t                  idle_threshold_ms;  /**< Idle ms before hold entry.      */
  uint32_t                  sw2_toggle_hold_ms; /**< SW2 long-hold threshold in ms.  */
  uint32_t                  max_park_iters;     /**< Bound forwarded to `park_m85`.  */
  uint32_t                  idle_ms;            /**< Accumulated idle time in ms.    */
  uint32_t                  sw2_hold_ms;        /**< Current continuous SW2 hold ms. */
  uint32_t                  handoff_count;      /**< Completed hold->wake cycles.    */
  er_lp_wake_reason_t       last_wake;          /**< Reason recorded on last wake.   */
  er_lp_state_t             state;              /**< Current ::er_lp_state_t.        */
  bool                      enabled;            /**< Runtime low-power toggle flag.  */
  bool                      prev_sw2_down;      /**< Previous sampled SW2 state.     */
  bool                      sw2_toggle_latched; /**< Suppresses short action on up.  */
  bool                      handoff_requested;  /**< Explicit immediate handoff req. */
} er_lp_ctx_t;

/**
 * @brief Initialise the low-power mailbox, launch M33, and prime state.
 *
 * @details Zeroes and stamps the shared mailbox at ``cfg->mailbox``, releases
 * CPU1 via ``cfg->ops.start_m33``, and sets initial enabled/active state.
 *
 * @param[out] ctx Caller-owned coordinator state.
 * @param[in]  cfg Non-NULL configuration descriptor.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Mailbox initialised and CPU1 started.
 * @retval k_ra8_err_null_ptr    NULL pointer or missing required callback.
 * @retval k_ra8_err_invalid_arg Invalid zero `max_park_iters` or `sw2_toggle_hold_ms`.
 *
 * @pre ``ctx``, ``cfg``, ``cfg->mailbox``, ``start_m33``, ``park_m85``, and
 *      ``notify_cpu1`` are non-NULL.
 * @post Mailbox carries `k_er_lp_mailbox_magic` and initial status.
 * @post ``ctx->state`` is active (if `start_enabled`) or disabled.
 * @note Not thread-safe; call once before ThreadX UI loop begins.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_init(er_lp_ctx_t* ctx, const er_lp_config_t* cfg);

/**
 * @brief Enable or disable low-power M33 hold-page mode at runtime.
 *
 * @param[in,out] ctx     Initialised coordinator state.
 * @param[in]     enabled `true` to enable M33 hold-page, `false` to stay on M85.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Mode updated and mailbox status notified.
 * @retval k_ra8_err_null_ptr    ``ctx`` was NULL.
 * @retval k_ra8_err_not_initialized ``ctx`` not initialised.
 *
 * @pre ``er_lowpower_init`` succeeded.
 * @post ``ctx->enabled`` equals ``enabled``; idle timer reset.
 * @note Not thread-safe; UI thread only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_set_enabled(er_lp_ctx_t* ctx, bool enabled);

/**
 * @brief Flip the runtime low-power toggle state and optionally read new value.
 *
 * @param[in,out] ctx         Initialised coordinator state.
 * @param[out]    out_enabled Optional output for new toggle state (may be NULL).
 *
 * @return ::ra8_err_t outcome from ``er_lowpower_set_enabled``.
 * @retval k_ra8_ok              Toggle applied.
 * @retval k_ra8_err_null_ptr    ``ctx`` was NULL.
 * @retval k_ra8_err_not_initialized ``ctx`` not initialised.
 *
 * @pre ``er_lowpower_init`` succeeded.
 * @post ``ctx->enabled`` inverted.
 * @note Not thread-safe; UI thread only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_toggle(er_lp_ctx_t* ctx, bool* out_enabled);

/**
 * @brief Trigger an explicit M85 -> M33 hold handoff on the next tick.
 *
 * @param[in,out] ctx Initialised coordinator state.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Handoff requested.
 * @retval k_ra8_err_null_ptr    ``ctx`` was NULL.
 * @retval k_ra8_err_not_initialized ``ctx`` not initialised.
 * @retval k_ra8_err_not_supported Low-power mode is currently disabled.
 *
 * @pre ``er_lowpower_init`` succeeded and low-power mode is enabled.
 * @post ``ctx->handoff_requested`` set when enabled.
 * @note Not thread-safe; UI thread only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_request_handoff(er_lp_ctx_t* ctx);

/**
 * @brief Advance button/idle state by ``dt_ms`` and emit next action.
 *
 * @details Differentiates short `SW2` taps (refresh-mode cycle) from long `SW2`
 * holds (`>= sw2_toggle_hold_ms`, low-power toggle), resets idle timer on any
 * user activity, and emits `k_er_lp_action_enter_hold` when enabled, M33 has
 * stamped `k_er_lp_m33_ready_sig`, and either an explicit handoff was requested
 * or idle time reached `idle_threshold_ms`.
 *
 * @param[in,out] ctx           Initialised coordinator state.
 * @param[in]     sw1_down      Current SW1 pressed level (`true` = held).
 * @param[in]     sw2_down      Current SW2 pressed level (`true` = held).
 * @param[in]     dt_ms         Elapsed milliseconds since previous tick.
 * @param[in]     user_activity True if touch or page draw occurred this tick.
 * @param[out]    out_action    Non-NULL output action for UI thread.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Action computed cleanly.
 * @retval k_ra8_err_null_ptr    NULL pointer argument.
 * @retval k_ra8_err_not_initialized ``ctx`` not initialised.
 * @retval k_ra8_err_invalid_arg ``dt_ms == 0``.
 *
 * @pre ``er_lowpower_init`` succeeded.
 * @post `*out_action` holds the highest-priority action for this tick.
 * @note Not thread-safe; UI thread only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_tick_inputs(er_lp_ctx_t*          ctx,
                                                bool                  sw1_down,
                                                bool                  sw2_down,
                                                uint32_t              dt_ms,
                                                bool                  user_activity,
                                                er_lp_input_action_t* out_action);

/**
 * @brief Hand off page hold to M33 and park M85 in Sleep until wakeup.
 *
 * @details Publishes ``held_page``, ``fb_base``, ``fb_crc`` and status
 * `k_er_lp_status_holding` into the mailbox, notifies CPU1, invokes optional
 * `before_park`, calls `ops.park_m85`, invokes optional `after_wake`, acks any
 * pending `turn_req`, updates `handoff_count` / `enabled` (if wake reason was
 * `k_er_lp_wake_toggle`), and returns `*out_wake`.
 *
 * @param[in,out] ctx       Initialised coordinator state.
 * @param[in]     held_page Page index held on panel during sleep.
 * @param[in]     fb_base   Active displayed SDRAM framebuffer address.
 * @param[in]     fb_crc    Fingerprint of active displayed page.
 * @param[out]    out_wake  Non-NULL wakeup reason reported back to UI thread.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Park/wake handoff finished cleanly.
 * @retval k_ra8_err_null_ptr    NULL pointer argument.
 * @retval k_ra8_err_not_initialized ``ctx`` not initialised or M33 not ready.
 * @retval k_ra8_err_not_supported Low-power mode disabled.
 *
 * @pre ``er_lowpower_init`` succeeded, low-power enabled, and M33 ready.
 * @post `turn_ack == turn_req`, `before_park`/`after_wake` balanced, `state` restored.
 * @note Not thread-safe; UI thread only.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_enter_hold(er_lp_ctx_t*         ctx,
                                               uint32_t             held_page,
                                               uint32_t             fb_base,
                                               uint32_t             fb_crc,
                                               er_lp_wake_reason_t* out_wake);

/**
 * @brief Query whether M33 hold-page low-power mode is currently enabled.
 *
 * @param[in] ctx Coordinator context (NULL-safe).
 * @return `true` if initialised and enabled, `false` otherwise.
 * @pre None.
 * @post Context state unchanged.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] bool er_lowpower_is_enabled(const er_lp_ctx_t* ctx);

/**
 * @brief Return current coordinator ::er_lp_state_t.
 *
 * @param[in] ctx Coordinator context (NULL-safe).
 * @return Current state (`k_er_lp_state_uninit` when NULL).
 * @pre None.
 * @post Context state unchanged.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] er_lp_state_t er_lowpower_state(const er_lp_ctx_t* ctx);

/**
 * @brief Return total completed M85->M33->M85 handoff cycles.
 *
 * @param[in] ctx Coordinator context (NULL-safe).
 * @return Count of completed handoffs (`0` when NULL).
 * @pre None.
 * @post Context state unchanged.
 * @note Pure read.
 * @since 0.1.0
 */
[[nodiscard]] uint32_t er_lowpower_handoff_count(const er_lp_ctx_t* ctx);

/** @brief J-Link / emulator probe: completed M85->M33->M85 handoff cycles. */
extern volatile uint32_t g_ns_lp_handoffs;

/** @brief J-Link / emulator probe: 1 when M33 low-power hold toggle is ON. */
extern volatile uint32_t g_ns_lp_enabled;

/** @brief J-Link / emulator probe: return status from ``er_lowpower_init``. */
extern volatile ra8_err_t g_ns_lp_init_err;

/**
 * @brief Initialise NS dual-core SRAM3 mailbox and release Cortex-M33 hold supervisor.
 *
 * @param[in] fb_base Active SDRAM framebuffer base address.
 * @return Nothing; updates ::g_ns_lp_init_err and ::g_ns_lp_enabled.
 * @pre NS substrate initialised and initial panel frame rendered.
 * @post CPU1 M33 is released and active if ``g_ns_lp_init_err == k_ra8_ok``.
 * @note Target NS build only; UI thread context.
 * @since 0.1.0
 */
void er_lowpower_ns_setup(uint32_t fb_base);

/**
 * @brief Advance NS low-power input debouncer/idle timers for one UI frame.
 *
 * @param[in]  sw1_down      Current SW1 level (`true` = held).
 * @param[in]  sw2_down      Current SW2 level (`true` = held).
 * @param[in]  dt_ms         Frame delta in milliseconds.
 * @param[in]  user_activity `true` if touch, button, or render activity occurred.
 * @param[out] out_action    Non-NULL output low-power action for the UI loop.
 * @return Nothing.
 * @pre ``er_lowpower_ns_setup`` completed.
 * @post Probe variables and LP toggle state updated.
 * @note Target NS build only; UI thread context.
 * @since 0.1.0
 */
void er_lowpower_ns_tick(bool                  sw1_down,
                         bool                  sw2_down,
                         uint32_t              dt_ms,
                         bool                  user_activity,
                         er_lp_input_action_t* out_action);

/**
 * @brief Hand off page hold to Cortex-M33, stop HOCO, park M85 in Sleep WFI, and return wake reason.
 *
 * @param[in]  held_page Active page/frame counter held on GLCDC.
 * @param[in]  fb_base   Active displayed SDRAM framebuffer address.
 * @param[in]  fb_crc    Current framebuffer fingerprint.
 * @param[out] out_wake  Non-NULL wake reason returned from CPU1 mailbox.
 * @return ::ra8_err_t outcome from ``er_lowpower_enter_hold``.
 * @pre ``er_lowpower_ns_setup`` completed.
 * @post NS SysTick restored, edge flags reset by caller, probes updated.
 * @note Target NS build only; UI thread context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_ns_handle_hold(uint32_t             held_page,
                                                   uint32_t             fb_base,
                                                   uint32_t             fb_crc,
                                                   er_lp_wake_reason_t* out_wake);

/**
 * @brief Request low-power hold handoff on the active Non-Secure target context.
 *
 * @details Forwards to ``er_lowpower_request_handoff(&s_ns_lp_ctx)`` so background
 * supervisor threads (e.g. system thread when the display backlight turns OFF)
 * can initiate an M85 -> M33 hold handoff without waiting for the full idle timer.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Handoff successfully requested.
 * @retval k_ra8_err_not_supported Low-power mode is disabled.
 * @retval k_ra8_err_not_initialized Context not initialised.
 *
 * @pre ``er_lowpower_ns_setup`` completed.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t er_lowpower_ns_request_handoff(void);

/**
 * @brief Query whether target M33 hold-page low-power mode is currently enabled.
 *
 * @return `true` if initialised and enabled, `false` otherwise.
 * @since 0.1.0
 */
[[nodiscard]] bool er_lowpower_ns_is_enabled(void);

#ifdef __cplusplus
}
#endif
