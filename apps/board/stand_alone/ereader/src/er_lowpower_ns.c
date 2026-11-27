/**
 * @file er_lowpower_ns.c
 * @brief Non-Secure target glue binding er_lowpower policy to NSC dual-core veneers
 * @ingroup grp_apps
 *
 * @par Tag
 * [Ring 6 / APP] {World: NS}
 *
 * @details
 * Keeps TrustZone NSC dual-core calls, ThreadX SysTick pause/resume around Sleep
 * WFI, and HIL probe variables isolated from `ns_main.c` so `ns_main.c` stays
 * focused on reader orchestration while sharing identical policy logic in
 * `er_lowpower.c` with host unit tests.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "er_lowpower.h"
#include "er_ns_nsc_guard.h"
#include "ra8_attributes.h"
#include "ra8_err.h"
#include "ra8_nsc.h"
#include "ra8_nsc_dualcore.h"

/** @brief Active low-power handoff state on M85 Non-Secure UI thread. */
static er_lp_ctx_t s_ns_lp_ctx = {};

/** @brief Saved Armv8-M SYST_CSR_NS value across Sleep WFI hold windows. */
static uint32_t s_ns_syst_csr_saved = 0U;

/** @brief J-Link / emulator probe: completed M85->M33->M85 handoff cycles. */
volatile uint32_t g_ns_lp_handoffs = 0U;

/** @brief J-Link / emulator probe: 1 when M33 low-power hold toggle is ON. */
volatile uint32_t g_ns_lp_enabled = 0U;

/** @brief J-Link / emulator probe: return status from ``er_lowpower_init``. */
volatile ra8_err_t g_ns_lp_init_err = k_ra8_ok;

/** @brief Armv8-M Non-Secure SysTick control register address. */
typedef enum : uintptr_t {
  k_ns_syst_csr_addr = 0xE000E010UL, /**< Armv8-M SYST_CSR_NS control register. */
} ns_lp_addr_t;

/** @brief Armv8-M Non-Secure SysTick masks and hold timing constants. */
typedef enum : uint32_t {
  k_ns_syst_irq_en_msk    = 0x00000003UL, /**< SYST_CSR.ENABLE | SYST_CSR.TICKINT.  */
  k_ns_syst_countflag_msk = 1UL << 16U,   /**< SYST_CSR.COUNTFLAG (W1C clear mask). */
  k_ns_lp_sw2_hold_ms     = 500U,         /**< SW2 hold duration to toggle LP mode. */
  k_ns_lp_park_iters      = 128U,         /**< Bounded Sleep WFI wait rounds.       */
#ifdef RA8_EREADER_LP_EMU_SOAK
  k_ns_lp_idle_ms    = 120U, /**< Accelerated idle handoff for soak.  */
  k_ns_lp_auto_polls = 24U,  /**< Scripted M33 wake cadence for soak. */
#else
  k_ns_lp_idle_ms    = 2000U, /**< Production idle threshold before park. */
  k_ns_lp_auto_polls = 0U,    /**< Production: button/touch wake only.    */
#endif
} ns_lp_const_t;

/** @brief Guarded NSC log helper preventing SysTick_NS preemption in Secure mode. */
RA8_INTERNAL static ra8_err_t internal_ns_lp_log_emit(const char* tag, const char* msg)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_log_emit(tag, msg));
  return err;
}

/** @brief NSC adapter: configure SRAM3/IPC/LED1 attribution and release CPU1. */
RA8_INTERNAL static ra8_err_t internal_ns_lp_start_m33(uintptr_t base, uintptr_t sp)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_dualcore_start_m33(base, sp));
  return err;
}

/** @brief NSC adapter: delegate SW1/SW2 NS, stop HOCO, and enter Sleep WFI. */
RA8_INTERNAL static ra8_err_t internal_ns_lp_park_m85(uint32_t max_iters, uint32_t* out_woken)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_dualcore_park_m85(max_iters, out_woken));
  return err;
}

/** @brief NSC adapter: raise IPC channel 2 interrupt toward CPU1 M33. */
RA8_INTERNAL static ra8_err_t internal_ns_lp_notify_cpu1(void)
{
  ra8_err_t err = k_ra8_ok;
  ER_NS_NSC_CALL(err, ra8_nsc_dualcore_notify_cpu1());
  return err;
}

/** @brief Drain/mask NS SysTick, refresh WDT, and pause SysTick prior to HOCO Sleep. */
RA8_INTERNAL static void internal_ns_lp_before_park(void)
{
  const uint32_t guard = er_ns_nsc_enter();
  /* ARMv8-M ARM B3.2.2 SYST_CSR_NS -- suspend ThreadX tick around Sleep WFI. */
  volatile uint32_t* const syst_csr = (volatile uint32_t*)k_ns_syst_csr_addr;
  s_ns_syst_csr_saved               = (*syst_csr) | guard;
  *syst_csr                         = s_ns_syst_csr_saved & ~(uint32_t)k_ns_syst_irq_en_msk;
  __asm__ volatile("dsb\n\tisb" ::: "memory");
  ra8_nsc_wdt_refresh();
}

/** @brief Refresh WDT while SysTick is paused, then resume NS SysTick after M85 exits Sleep. */
RA8_INTERNAL static void internal_ns_lp_after_wake(void)
{
  ra8_nsc_wdt_refresh();
  /* ARMv8-M ARM B3.2.2 SYST_CSR_NS -- resume NS ThreadX tick on M85 wakeup. */
  volatile uint32_t* const syst_csr = (volatile uint32_t*)k_ns_syst_csr_addr;
  *syst_csr =
    (s_ns_syst_csr_saved & ~(uint32_t)k_ns_syst_countflag_msk) | (uint32_t)k_ns_syst_irq_en_msk;
  __asm__ volatile("dsb\n\tisb" ::: "memory");
}

void er_lowpower_ns_setup(uint32_t fb_base)
{
  const er_lp_config_t cfg = {
    .mailbox            = (volatile er_lp_mailbox_t*)(uintptr_t)k_er_lp_mailbox_ns_addr,
    .ops                = {.start_m33   = internal_ns_lp_start_m33,
                           .park_m85    = internal_ns_lp_park_m85,
                           .notify_cpu1 = internal_ns_lp_notify_cpu1,
                           .before_park = internal_ns_lp_before_park,
                           .after_wake  = internal_ns_lp_after_wake},
    .idle_threshold_ms  = (uint32_t)k_ns_lp_idle_ms,
    .sw2_toggle_hold_ms = (uint32_t)k_ns_lp_sw2_hold_ms,
    .max_park_iters     = (uint32_t)k_ns_lp_park_iters,
    .auto_turn_polls    = (uint32_t)k_ns_lp_auto_polls,
    .initial_fb_base    = fb_base,
    .start_enabled      = true,
  };
  g_ns_lp_init_err = er_lowpower_init(&s_ns_lp_ctx, &cfg);
  g_ns_lp_enabled  = er_lowpower_is_enabled(&s_ns_lp_ctx) ? 1U : 0U;
  if (g_ns_lp_init_err == k_ra8_ok) {
    (void)internal_ns_lp_log_emit("LP", "M33 secondary hold supervisor started");
  } else {
    (void)internal_ns_lp_log_emit("LP", "M33 secondary start failed");
  }
}

void er_lowpower_ns_tick(bool                  sw1_down,
                         bool                  sw2_down,
                         uint32_t              dt_ms,
                         bool                  user_activity,
                         er_lp_input_action_t* out_action)
{
  if (out_action == nullptr) {
    return;
  }
  *out_action = k_er_lp_action_none;
  (void)er_lowpower_tick_inputs(&s_ns_lp_ctx, sw1_down, sw2_down, dt_ms, user_activity, out_action);
  if (*out_action == k_er_lp_action_toggled) {
    g_ns_lp_enabled = er_lowpower_is_enabled(&s_ns_lp_ctx) ? 1U : 0U;
    (void)internal_ns_lp_log_emit("LP",
                                  (g_ns_lp_enabled != 0U) ? "Low-power hold toggled ON"
                                                          : "Low-power hold toggled OFF");
  }
}

ra8_err_t er_lowpower_ns_handle_hold(uint32_t             held_page,
                                     uint32_t             fb_base,
                                     uint32_t             fb_crc,
                                     er_lp_wake_reason_t* out_wake)
{
  if (out_wake == nullptr) {
    return k_ra8_err_null_ptr;
  }
  *out_wake = k_er_lp_wake_none;
  (void)internal_ns_lp_log_emit("LP", "Entering M33 hold (M85 HOCO stop + WFI)");
  const ra8_err_t err = er_lowpower_enter_hold(&s_ns_lp_ctx, held_page, fb_base, fb_crc, out_wake);
  g_ns_lp_handoffs    = er_lowpower_handoff_count(&s_ns_lp_ctx);
  g_ns_lp_enabled     = er_lowpower_is_enabled(&s_ns_lp_ctx) ? 1U : 0U;
  if (err != k_ra8_ok) {
    (void)internal_ns_lp_log_emit("LP", "M85 park error");
    return err;
  }
  if (*out_wake == k_er_lp_wake_toggle) {
    (void)internal_ns_lp_log_emit("LP", "M33 SW2 wake -> low-power OFF");
  }
  return k_ra8_ok;
}

ra8_err_t er_lowpower_ns_request_handoff(void)
{
  return er_lowpower_request_handoff(&s_ns_lp_ctx);
}

bool er_lowpower_ns_is_enabled(void)
{
  return er_lowpower_is_enabled(&s_ns_lp_ctx);
}
