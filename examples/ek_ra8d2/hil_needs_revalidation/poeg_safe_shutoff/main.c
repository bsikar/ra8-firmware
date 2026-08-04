/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/poeg_safe_shutoff/main.c
 * @brief POEG (Port Output Enable for GPT) safe-shutoff demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * POEG forces the outputs of a GPT channel into a safe (high-impedance) state
 * WITHOUT CPU involvement the moment a fault is detected -- the mechanism a
 * motor-control H-bridge relies on so an over-current trip kills the PWM in a
 * single cycle. This demo drives a GPT PWM output, then exercises the full
 * safe-shutoff cycle every second:
 *
 *   1. RUN     -- GPT0 runs in saw-wave PWM; POEGG.ST (output-disable STATE) is
 *                 clear, so the GPT outputs are driven.
 *   2. SHUTOFF -- assert the software-stop request (``ra8_poeg_trigger_stop``,
 *                 POEGG.SSF = 1). The block latches POEGG.ST = 1 and the GPT
 *                 outputs go high-impedance. Verified by reading ST back.
 *   3. RE-ENABLE - clear the request (``ra8_poeg_clear_status`` of SSF). With no
 *                 request left, POEGG.ST returns to 0 and the outputs are driven
 *                 again. Verified by reading ST back.
 *
 * The GPT COUNTER keeps advancing across the shutoff (POEG gates the output
 * pins, not the counter), so the demo also samples GTCNT before and after the
 * hold to prove the timer is really running.
 *
 * Bring-up: CGC + SysTick + MSTP + SCI console + LEDs, then GPT0 (saw PWM,
 * auto-start) and POEG group 0. Each pass reports
 * ``"poeg: pwm=run shutoff=highZ reenable=on ok=Y\r\n"`` on the J-Link OB CDC
 * channel; LED1 toggles on a healthy cycle, LED2 on a fault.
 *
 * On silicon the external fault input (GTETRG pin) additionally routes to the
 * POEG group when ``enable_pin`` is set and the GPT output-disable request is
 * wired to the group; this demo proves the request / STATE / clear path plus a
 * live PWM using the deterministic software trigger, which is what the headless
 * ra8_emulator gate can verify with no physical pin to scope.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` models the POEG
 * register file (``board_periph_poeg.c``): a POEGG.SSF write latches the derived
 * output-disable STATE flag POEGG.ST, and clearing every request flag returns
 * ST to 0, so ``ra8_poeg_get_status`` observes the shutoff and the re-enable
 * exactly as the hardware STATE latch would. The ``ra8_emulator_smoke.sh`` /
 * ``eil_all.sh`` gate keys on the ``ok=Y`` banner. Bare EK-RA8D2; no shields.
 *
 * @since 0.1.0
 */

#include <stddef.h>
#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_gpt.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_poeg.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "poeg_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_poeg_demo_baud      = 115200U,     /**< SCI console baud rate.          */
  k_poeg_demo_period_ms = 1000U,       /**< Delay between shutoff cycles.   */
  k_poeg_demo_settle_ms = 2U,          /**< High-Z hold while the PWM runs. */
  k_poeg_demo_gpt_prd   = 0x0000FFFFU, /**< 16-bit GPT period (saw PWM).    */
  k_poeg_demo_gpt_duty  = 0x00008000U, /**< Mid-scale PWM duty (50%).       */
} poeg_demo_config_t;

/** @brief GPT channel + POEG group the demo pairs. */
typedef enum : uint8_t {
  k_poeg_demo_gpt_channel = 0U, /**< GPT channel driving the PWM. */
  k_poeg_demo_group       = 0U, /**< POEG group gating GPT0.      */
} poeg_demo_unit_t;

/** @brief Output line tags. */
static const uint8_t k_poeg_demo_ok_msg[]  = "poeg: pwm=run shutoff=highZ reenable=on ok=Y\r\n";
static const uint8_t k_poeg_demo_bad_msg[] = "poeg: pwm=? shutoff=? reenable=? ok=N\r\n";

/**
 * @var g_poeg_shutoffs
 * @brief Count of healthy safe-shutoff cycles completed (assert + re-enable).
 * @note Read externally only (HIL / board emulator SWD probe).
 * @since 0.1.0
 */
volatile uint32_t g_poeg_shutoffs = 0U;

/**
 * @var g_poeg_last_status
 * @brief Last POEGG snapshot latched immediately after the shutoff assert.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_poeg_last_status = 0U;

/**
 * @var g_poeg_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_poeg_heartbeat = 0U;

/**
 * @var g_poeg_mismatch
 * @brief Latches the count of cycles whose verdict was not healthy.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_poeg_mismatch = 0U;

/** @brief Park forever after a fatal init failure. */
static void poeg_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + MSTP + SCI console + LEDs up. */
static void poeg_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_poeg_demo_baud) != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
}

/**
 * @brief Configure GPT0 for saw-wave PWM with ``auto_start = true``.
 *
 * @details
 * Uses the same full-descriptor path the gpt_pwm_demo uses so the GPT counter
 * advances immediately and drives a mid-scale PWM the POEG group can gate.
 *
 * @return ``ra8_err_t`` from ``ra8_gpt_init``.
 * @retval k_ra8_ok GPT0 armed and counting.
 *
 * @pre CGC + MSTP are up.
 * @pre IRQs masked or single-threaded init.
 * @post On success GPT0 counts in saw-PWM at a 50% duty.
 * @post GTPR / GTCCRA reflect the demo period + duty.
 *
 * @par MC/DC:
 * Single decision ``ra8_gpt_init != ok`` at the call site -- 2 vectors
 * (golden + the null-cfg / bad-channel rejects covered in the host test).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t poeg_demo_arm_gpt(void)
{
  const ra8_gpt_cfg_t cfg = {
    .mode       = k_ra8_gpt_mode_saw_pwm,
    .prescaler  = k_ra8_gpt_ps_div_4,
    .period     = (uint32_t)k_poeg_demo_gpt_prd,
    .duty_a     = (uint32_t)k_poeg_demo_gpt_duty,
    .duty_b     = (uint32_t)k_poeg_demo_gpt_duty,
    .auto_start = true,
  };
  return ra8_gpt_init((uint8_t)k_poeg_demo_gpt_channel, &cfg);
}

/**
 * @brief Arm POEG group 0 to accept an output-disable request.
 *
 * @details
 * Enables the external GTETRG fault-input path (``enable_pin``) so a real over-
 * current comparator wired to the POEG pin would trip the shutoff on silicon;
 * the demo itself drives the deterministic software trigger. No output-short /
 * oscillation-stop sources are armed.
 *
 * @return ``ra8_err_t`` from ``ra8_poeg_init``.
 * @retval k_ra8_ok POEG group 0 powered and armed.
 *
 * @pre ``ra8_mstp_init`` succeeded.
 * @pre IRQs masked or single-threaded init.
 * @post POEG group 0 MSTP reference is held.
 * @post POEGG.PIDE reflects the enabled external-input path.
 *
 * @par MC/DC:
 * Single decision ``ra8_poeg_init != ok`` at the call site -- 2 vectors
 * (golden + the null-cfg / bad-group rejects covered in the host test).
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t poeg_demo_arm_poeg(void)
{
  const ra8_poeg_cfg_t cfg = {
    .enable_pin      = true,
    .enable_ioc      = false,
    .enable_osc_stop = false,
    .invert_input    = false,
  };
  return ra8_poeg_init((uint8_t)k_poeg_demo_group, &cfg);
}

/**
 * @brief Run one safe-shutoff cycle and fold the result into the verdict.
 *
 * @param[out] out_ok 1 when the whole assert -> high-Z -> clear -> re-enable
 *                    cycle behaved AND the PWM counter advanced.
 *
 * @details
 * Samples GTCNT, asserts the POEG software stop (SSF), confirms the derived
 * output-disable STATE flag (POEGG.ST) latched high (outputs high-impedance),
 * holds the shutoff for ``k_poeg_demo_settle_ms`` while the counter keeps
 * running, clears the request, confirms ST returned to 0 (outputs re-enabled),
 * then re-samples GTCNT. Healthy iff the shutoff latched, the re-enable
 * cleared it, AND GTCNT advanced across the hold.
 *
 * @return ``ra8_err_t`` -- ``k_ra8_ok`` once the (bounded) cycle finishes, or the
 *         first driver error encountered.
 * @retval k_ra8_ok        Cycle ran; ``*out_ok`` is 0 or 1.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 *
 * @pre ::poeg_demo_arm_gpt and ::poeg_demo_arm_poeg succeeded.
 * @pre GPT0 is counting.
 * @post ``g_poeg_last_status`` holds the post-assert POEGG snapshot.
 * @post ``*out_ok`` is 0 or 1.
 *
 * @par MC/DC:
 * Decision ``ok = shutoff && reenabled && running`` (3 conditions). The host
 * test supplies N+1 = 4 vectors, varying each condition independently.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t poeg_demo_cycle(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");
  *out_ok = 0U;

  /* RUN: sample the free-running PWM counter. */
  uint32_t        before = 0U;
  const ra8_err_t rd0    = ra8_gpt_read((uint8_t)k_poeg_demo_gpt_channel, &before);
  if (rd0 != k_ra8_ok) {
    return rd0;
  }

  /* SHUTOFF: assert the software output-disable request and read ST back. */
  const ra8_err_t terr = ra8_poeg_trigger_stop((uint8_t)k_poeg_demo_group);
  if (terr != k_ra8_ok) {
    return terr;
  }
  uint32_t        asserted = 0U;
  const ra8_err_t serr     = ra8_poeg_get_status((uint8_t)k_poeg_demo_group, &asserted);
  if (serr != k_ra8_ok) {
    return serr;
  }
  g_poeg_last_status    = asserted;
  const uint8_t shutoff = ((asserted & (uint32_t)k_ra8_poeg_status_st) != 0U) ? 1U : 0U;

  /* Hold the shutoff briefly; the GPT counter keeps advancing meanwhile. */
  ra8_delay_ms((uint32_t)k_poeg_demo_settle_ms);

  /* RE-ENABLE: clear the request and confirm ST fell back to 0. */
  const ra8_err_t cerr =
    ra8_poeg_clear_status((uint8_t)k_poeg_demo_group, (uint32_t)k_ra8_poeg_status_ssf);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  uint32_t        cleared = 0U;
  const ra8_err_t cs      = ra8_poeg_get_status((uint8_t)k_poeg_demo_group, &cleared);
  if (cs != k_ra8_ok) {
    return cs;
  }
  const uint8_t reenabled = ((cleared & (uint32_t)k_ra8_poeg_status_st) == 0U) ? 1U : 0U;

  /* RUN confirm: the PWM counter must have advanced across the hold. */
  uint32_t        after = 0U;
  const ra8_err_t rd1   = ra8_gpt_read((uint8_t)k_poeg_demo_gpt_channel, &after);
  if (rd1 != k_ra8_ok) {
    return rd1;
  }
  const uint8_t running = (after != before) ? 1U : 0U;

  *out_ok = (shutoff != 0U && reenabled != 0U && running != 0U) ? 1U : 0U;
  return k_ra8_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  poeg_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the SysTick
   * path (ra8_emulator does not advance DWT_CYCCNT). No NVIC sources are armed. */
  ra8_isr_globals_enable();

  if (poeg_demo_arm_gpt() != k_ra8_ok) {
    poeg_demo_panic_halt();
  }
  if (poeg_demo_arm_poeg() != k_ra8_ok) {
    poeg_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = poeg_demo_cycle(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_poeg_demo_ok_msg,
                                         (size_t)(sizeof(k_poeg_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
      ++g_poeg_shutoffs;
    } else {
      (void)ra8_board_uart_console_write(k_poeg_demo_bad_msg,
                                         (size_t)(sizeof(k_poeg_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
      ++g_poeg_mismatch;
    }
    ++g_poeg_heartbeat;
    ra8_delay_ms((uint32_t)k_poeg_demo_period_ms);
  }
  poeg_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
