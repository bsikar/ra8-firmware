/**
 * @file er_lowpower.c
 * @brief Non-Secure M85 low-power coordinator for e-reader M33 page-hold handoff
 * @ingroup grp_apps
 *
 * @par Tag
 * [App / Product] {World: NS}
 *
 * @details
 * Implements the pure-policy and dependency-injected platform coordination logic
 * declared in ``er_lowpower.h``. All hardware TrustZone transitions run on the
 * Secure side via caller-injected NSC veneers; the controller itself contains
 * zero platform-specific assembly or host-mock conditionals so host unit tests
 * achieve complete MC/DC coverage of every branch.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "er_lowpower.h"

#include <stddef.h>

#include "ra8_attributes.h"
#include "ra8_check.h"

/** @brief Diagnostic log tag for e-reader low-power coordinator. */
static const char s_tag[] = "ER_LP";

/** @brief Fixed upper bounds for input validation. */
typedef enum : uint32_t {
  k_er_lp_max_park_iters_cap = 4096U,  /**< Bounds Sleep WFI spin count.      */
  k_er_lp_max_dt_ms          = 60000U, /**< Maximum single UI tick step (ms). */
} er_lp_limits_t;

/**
 * @brief Publish active/disabled mailbox status when enabled flag changes.
 *
 * @param[in,out] ctx Initialised coordinator context.
 *
 * @pre ``ctx != nullptr`` and ``ctx->mailbox != nullptr``.
 * @post ``ctx->mailbox->status`` reflects ``ctx->enabled``.
 * @note Invokes ``notify_cpu1`` if available so M33 observes status immediately.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_lp_sync_status(er_lp_ctx_t* ctx)
{
  ctx->mailbox->status =
    ctx->enabled ? (uint32_t)k_er_lp_status_turning : (uint32_t)k_er_lp_status_disabled;
  if (ctx->ops.notify_cpu1 != nullptr) {
    (void)ctx->ops.notify_cpu1();
  }
}

/**
 * @brief Reconcile mailbox wake flags after `park_m85` returns to M85.
 *
 * @param[in,out] ctx   Coordinator context.
 * @param[in]     woken Nonzero when Secure `IPC0` IRQ0 flagged a wake.
 *
 * @return Resolved ::er_lp_wake_reason_t for the UI loop.
 * @pre ``ctx != nullptr`` and ``ctx->mailbox != nullptr``.
 * @post ``mailbox->turn_ack == mailbox->turn_req``; ``ctx->state`` updated.
 * @note Flips ``ctx->enabled`` to false when `k_er_lp_wake_toggle` woke M85.
 * @since 0.1.0
 */
RA8_INTERNAL static er_lp_wake_reason_t internal_lp_finish_wake(er_lp_ctx_t* ctx, uint32_t woken)
{
  volatile er_lp_mailbox_t* const mbox = ctx->mailbox;
  er_lp_wake_reason_t             wake = k_er_lp_wake_none;

  const uint32_t req = mbox->turn_req;
  if ((woken != 0U) || (req != mbox->turn_ack)) {
    const uint32_t raw_reason = mbox->wake_reason;
    wake                      = ((raw_reason >= (uint32_t)k_er_lp_wake_next_page) &&
                                 (raw_reason <= (uint32_t)k_er_lp_wake_auto_test))
                                  ? (er_lp_wake_reason_t)raw_reason
                                  : k_er_lp_wake_next_page;
    mbox->turn_ack            = req;
    if (wake == k_er_lp_wake_toggle) {
      ctx->enabled = false;
    }
  }

  ctx->last_wake         = wake;
  ctx->idle_ms           = 0U;
  ctx->handoff_requested = false;
  ctx->handoff_count += 1U;
  ctx->state = ctx->enabled ? k_er_lp_state_active : k_er_lp_state_disabled;
  mbox->status =
    ctx->enabled ? (uint32_t)k_er_lp_status_turning : (uint32_t)k_er_lp_status_disabled;
  return wake;
}

[[nodiscard]] ra8_err_t er_lowpower_init(er_lp_ctx_t* ctx, const er_lp_config_t* cfg)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "init: ctx");
  RA8_CHECK_NULL_PTR(cfg, s_tag, "init: cfg");
  RA8_CHECK_NULL_PTR(cfg->mailbox, s_tag, "init: mailbox");
  RA8_CHECK_NULL_PTR(cfg->ops.start_m33, s_tag, "init: start_m33");
  RA8_CHECK_NULL_PTR(cfg->ops.park_m85, s_tag, "init: park_m85");
  RA8_CHECK_NULL_PTR(cfg->ops.notify_cpu1, s_tag, "init: notify_cpu1");
  RA8_CHECK_RANGE(cfg->max_park_iters,
                  1U,
                  (uint32_t)k_er_lp_max_park_iters_cap,
                  k_ra8_err_invalid_arg);
  RA8_CHECK_RANGE(cfg->sw2_toggle_hold_ms, 1U, (uint32_t)k_er_lp_max_dt_ms, k_ra8_err_invalid_arg);

  *ctx = (er_lp_ctx_t){
    .mailbox            = cfg->mailbox,
    .ops                = cfg->ops,
    .idle_threshold_ms  = cfg->idle_threshold_ms,
    .sw2_toggle_hold_ms = cfg->sw2_toggle_hold_ms,
    .max_park_iters     = cfg->max_park_iters,
    .enabled            = cfg->start_enabled,
    .state              = cfg->start_enabled ? k_er_lp_state_active : k_er_lp_state_disabled,
  };

  const ra8_err_t err =
    ctx->ops.start_m33((uintptr_t)k_er_lp_cpu1_mram_base, (uintptr_t)k_er_lp_cpu1_stack_top);
  if (err != k_ra8_ok) {
    ctx->state = k_er_lp_state_uninit;
    return err;
  }

  /* Touch SRAM3 NS mailbox only after start_m33 opens SRAMSABAR3 to Non-Secure. */
  ctx->mailbox->fb_base         = cfg->initial_fb_base;
  ctx->mailbox->fb_crc          = 0U;
  ctx->mailbox->held_page       = 0U;
  ctx->mailbox->turn_req        = 0U;
  ctx->mailbox->turn_ack        = 0U;
  ctx->mailbox->wake_reason     = (uint32_t)k_er_lp_wake_none;
  ctx->mailbox->auto_turn_polls = cfg->auto_turn_polls;
  ctx->mailbox->status =
    cfg->start_enabled ? (uint32_t)k_er_lp_status_booting : (uint32_t)k_er_lp_status_disabled;
  ctx->mailbox->magic = (uint32_t)k_er_lp_mailbox_magic;
  (void)ctx->ops.notify_cpu1();
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t er_lowpower_set_enabled(er_lp_ctx_t* ctx, bool enabled)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "set_enabled: ctx");
  RA8_CHECK_RANGE((uint32_t)ctx->state,
                  (uint32_t)k_er_lp_state_active,
                  (uint32_t)k_er_lp_state_disabled,
                  k_ra8_err_not_initialized);

  ctx->enabled           = enabled;
  ctx->idle_ms           = 0U;
  ctx->handoff_requested = false;
  ctx->state             = enabled ? k_er_lp_state_active : k_er_lp_state_disabled;
  internal_lp_sync_status(ctx);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t er_lowpower_toggle(er_lp_ctx_t* ctx, bool* out_enabled)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "toggle: ctx");
  RA8_CHECK_RANGE((uint32_t)ctx->state,
                  (uint32_t)k_er_lp_state_active,
                  (uint32_t)k_er_lp_state_disabled,
                  k_ra8_err_not_initialized);

  const bool      next = !ctx->enabled;
  const ra8_err_t err  = er_lowpower_set_enabled(ctx, next);
  if ((err == k_ra8_ok) && (out_enabled != nullptr)) {
    *out_enabled = next;
  }
  return err;
}

[[nodiscard]] ra8_err_t er_lowpower_request_handoff(er_lp_ctx_t* ctx)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "request_handoff: ctx");
  RA8_CHECK_RANGE((uint32_t)ctx->state,
                  (uint32_t)k_er_lp_state_active,
                  (uint32_t)k_er_lp_state_disabled,
                  k_ra8_err_not_initialized);

  if (!ctx->enabled) {
    return k_ra8_err_not_supported;
  }
  ctx->handoff_requested = true;
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t er_lowpower_tick_inputs(er_lp_ctx_t*          ctx,
                                                bool                  sw1_down,
                                                bool                  sw2_down,
                                                uint32_t              dt_ms,
                                                bool                  user_activity,
                                                er_lp_input_action_t* out_action)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "tick_inputs: ctx");
  RA8_CHECK_NULL_PTR(out_action, s_tag, "tick_inputs: out_action");
  RA8_CHECK_RANGE((uint32_t)ctx->state,
                  (uint32_t)k_er_lp_state_active,
                  (uint32_t)k_er_lp_state_disabled,
                  k_ra8_err_not_initialized);
  RA8_CHECK_RANGE(dt_ms, 1U, (uint32_t)k_er_lp_max_dt_ms, k_ra8_err_invalid_arg);

  *out_action = k_er_lp_action_none;

  if (sw2_down) {
    ctx->idle_ms = 0U;
    ctx->sw2_hold_ms += dt_ms;
    if (!ctx->sw2_toggle_latched && (ctx->sw2_hold_ms >= ctx->sw2_toggle_hold_ms)) {
      ctx->sw2_toggle_latched = true;
      ctx->prev_sw2_down      = true;
      (void)er_lowpower_toggle(ctx, nullptr);
      *out_action = k_er_lp_action_toggled;
      return k_ra8_ok;
    }
    ctx->prev_sw2_down = true;
    return k_ra8_ok;
  }

  if (ctx->prev_sw2_down) {
    const bool emit_short   = !ctx->sw2_toggle_latched;
    ctx->prev_sw2_down      = false;
    ctx->sw2_hold_ms        = 0U;
    ctx->sw2_toggle_latched = false;
    ctx->idle_ms            = 0U;
    if (emit_short) {
      *out_action = k_er_lp_action_sw2_short;
      return k_ra8_ok;
    }
  }

  if (sw1_down || user_activity) {
    ctx->idle_ms           = 0U;
    ctx->handoff_requested = false;
    return k_ra8_ok;
  }

  if (!ctx->enabled) {
    return k_ra8_ok;
  }

  if (ctx->idle_threshold_ms > 0U) {
    ctx->idle_ms += dt_ms;
  }
  const bool due = ctx->handoff_requested ||
                   ((ctx->idle_threshold_ms > 0U) && (ctx->idle_ms >= ctx->idle_threshold_ms));
  if (due && (ctx->mailbox->m33_sig == (uint32_t)k_er_lp_m33_ready_sig)) {
    *out_action = k_er_lp_action_enter_hold;
  }
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t er_lowpower_enter_hold(er_lp_ctx_t*         ctx,
                                               uint32_t             held_page,
                                               uint32_t             fb_base,
                                               uint32_t             fb_crc,
                                               er_lp_wake_reason_t* out_wake)
{
  RA8_CHECK_NULL_PTR(ctx, s_tag, "enter_hold: ctx");
  RA8_CHECK_NULL_PTR(out_wake, s_tag, "enter_hold: out_wake");
  RA8_CHECK_RANGE((uint32_t)ctx->state,
                  (uint32_t)k_er_lp_state_active,
                  (uint32_t)k_er_lp_state_disabled,
                  k_ra8_err_not_initialized);

  *out_wake = k_er_lp_wake_none;
  if (!ctx->enabled) {
    return k_ra8_err_not_supported;
  }
  if (ctx->mailbox->m33_sig != (uint32_t)k_er_lp_m33_ready_sig) {
    return k_ra8_err_not_initialized;
  }

  ctx->mailbox->held_page   = held_page;
  ctx->mailbox->fb_base     = fb_base;
  ctx->mailbox->fb_crc      = fb_crc;
  ctx->mailbox->wake_reason = (uint32_t)k_er_lp_wake_none;
  ctx->mailbox->status      = (uint32_t)k_er_lp_status_holding;
  ctx->state                = k_er_lp_state_holding;
  (void)ctx->ops.notify_cpu1();

  if (ctx->ops.before_park != nullptr) {
    ctx->ops.before_park();
  }
  uint32_t        woken = 0U;
  const ra8_err_t err   = ctx->ops.park_m85(ctx->max_park_iters, &woken);
  if (ctx->ops.after_wake != nullptr) {
    ctx->ops.after_wake();
  }

  *out_wake = internal_lp_finish_wake(ctx, woken);
  return err;
}

[[nodiscard]] bool er_lowpower_is_enabled(const er_lp_ctx_t* ctx)
{
  if ((ctx == nullptr) || (ctx->state == k_er_lp_state_uninit)) {
    return false;
  }
  return ctx->enabled;
}

[[nodiscard]] er_lp_state_t er_lowpower_state(const er_lp_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return k_er_lp_state_uninit;
  }
  return ctx->state;
}

[[nodiscard]] uint32_t er_lowpower_handoff_count(const er_lp_ctx_t* ctx)
{
  if (ctx == nullptr) {
    return 0U;
  }
  return ctx->handoff_count;
}
