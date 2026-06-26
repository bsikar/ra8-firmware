/**
 * @file examples/ek_ra8d2/pdg_delay_demo/main.c
 * @brief PWM Delay Generation (PDG) bring-up + delay-program demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The PDG block adds a fine, DLL-derived delay (DLY[6:0], 1/128 of the GPT
 * core-clock period per step in the 80..160 MHz band) to the rising and
 * falling edges of a GPT32 channel's GTIOCnA / GTIOCnB outputs, so a PWM
 * edge can be nudged with sub-nanosecond resolution. This demo brings the
 * PDG DLL up on channel 0, programs a mid-range delay code (0x40) on the
 * GTIOC0A rising edge, reads it back, and reports the configured state.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + MSTP. Once a second the loop
 * reads back the delay code + PDG status and reports
 * ``"pdg: dll=on ch0=on delay=0x40 cfg=ok\r\n"`` on the J-Link OB CDC
 * channel. LED1 toggles while the configuration reads back clean; LED2
 * toggles otherwise.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **What headless validation can and cannot show.** The PDG has no
 * software-readable "edge was delayed" status -- its only observable
 * effect is the *timing* of a GPT output edge, which needs a logic
 * analyzer / oscilloscope (and a running GPT32_0 PWM source) to measure.
 * ``tools/board_sim`` shadows the PDG control + delay registers, so the
 * *bring-up + delay-program + read-back* path runs and reports
 * ``cfg=ok`` (the ``board_sim_smoke.sh`` gate keys on it), but the actual
 * delay generation cannot be confirmed without silicon + an instrument. This
 * app therefore lives in ``hw_pending/``; see ``README.md`` for the on-scope
 * bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_pdg.h"
#include "ra_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "pdg_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_pdg_demo_baud      = 115200U, /**< SCI8 baud rate.        */
  k_pdg_demo_period_ms = 1000U,   /**< Delay between reports. */
} pdg_demo_config_t;

/** @brief PDG channel + delay programmed by this demo. */
typedef enum : uint8_t {
  k_pdg_demo_channel    = 0U,    /**< PDG / GPT32 channel 0.           */
  k_pdg_demo_chan_mask  = 0x01U, /**< channel_mask bit 0.              */
  k_pdg_demo_delay_code = 0x40U, /**< Mid-range DLY[6:0] code (0..7F). */
} pdg_demo_chan_t;

/** @brief Output line tags. */
static const uint8_t k_pdg_demo_ok_msg[]  = "pdg: dll=on ch0=on delay=0x40 cfg=ok\r\n";
static const uint8_t k_pdg_demo_bad_msg[] = "pdg: cfg=BAD\r\n";

/**
 * @var g_pdg_cfg_ok
 * @brief 1 when the DLL + channel + delay read back as configured.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_pdg_cfg_ok = 0U;

/**
 * @var g_pdg_delay_readback
 * @brief Last delay code read back from the PDG temporary register.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_pdg_delay_readback = 0U;

/**
 * @var g_pdg_dll
 * @brief 1 when GTDLYCR.DLLEN reads back enabled.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_pdg_dll = 0U;

/**
 * @var g_pdg_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_pdg_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void pdg_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void pdg_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_board_uart_console_init((uint32_t)k_pdg_demo_baud) != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    pdg_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    pdg_demo_panic_halt();
  }
}

/**
 * @brief Bring up the PDG DLL on channel 0 and programme a delay code.
 *
 * @details
 * Configures the 80..160 MHz FRANGE band, enables PDG channel 0, then
 * writes a mid-range delay (0x40) onto the GTIOC0A rising edge. The code
 * lands in the PDG temporary register and would propagate to the live
 * delay on the next GPT overflow/underflow once a GPT32_0 PWM source is
 * running (out of scope here -- see the bench plan).
 *
 * @par MC/DC:
 * Single decision ``err != k_ra_ok`` per call -- no compound condition.
 *
 * @return ``ra_err_t`` from ``ra_pdg_init`` / ``ra_pdg_set_delay``.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @post PDG DLL enabled, channel 0 un-bypassed, delay code staged.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t pdg_demo_configure(void)
{
  const ra_pdg_config_t cfg = {
    .frange       = k_ra_pdg_frange_80_160_mhz,
    .channel_mask = (uint8_t)k_pdg_demo_chan_mask,
    .auto_tune    = 0U,
    .gptclk_hz    = 0U,
  };
  const ra_err_t ierr = ra_pdg_init(&cfg);
  if (ierr != k_ra_ok) {
    return ierr;
  }
  return ra_pdg_set_delay((uint8_t)k_pdg_demo_channel,
                          k_ra_pdg_pin_a,
                          k_ra_pdg_edge_rising,
                          (uint8_t)k_pdg_demo_delay_code);
}

/**
 * @brief Read back the delay + PDG status and fold into the verdict.
 *
 * @param[out] out_ok 1 when the DLL is enabled, channel 0 is powered and
 *                    un-bypassed, and the delay reads back as programmed.
 *
 * @par MC/DC:
 * Decision ``ok = readback_match && dll && powered && bypass_off`` (4
 * conditions). The host test supplies N+1 = 5 vectors, varying each
 * condition independently.
 *
 * @return ``ra_err_t`` from the read-back accessors.
 * @retval k_ra_err_null_ptr ``out_ok`` was NULL.
 * @pre ::pdg_demo_configure succeeded.
 * @post ``g_pdg_delay_readback`` / ``g_pdg_dll`` updated.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t pdg_demo_sample(uint8_t* out_ok)
{
  RA_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  uint8_t        code = 0U;
  const ra_err_t derr =
    ra_pdg_get_delay((uint8_t)k_pdg_demo_channel, k_ra_pdg_pin_a, k_ra_pdg_edge_rising, &code);
  if (derr != k_ra_ok) {
    return derr;
  }
  ra_pdg_status_full_t st  = {};
  const ra_err_t       err = ra_pdg_get_status_full(&st);
  if (err != k_ra_ok) {
    return err;
  }
  g_pdg_delay_readback = (uint32_t)code;
  g_pdg_dll            = (st.dll_enabled != 0U) ? 1U : 0U;

  const uint8_t match    = (code == (uint8_t)k_pdg_demo_delay_code) ? 1U : 0U;
  const uint8_t powered  = (st.per_channel_powered[k_pdg_demo_channel] != 0U) ? 1U : 0U;
  const uint8_t bypass_n = (st.per_channel_bypass_off[k_pdg_demo_channel] != 0U) ? 1U : 0U;
  *out_ok = (match != 0U && st.dll_enabled != 0U && powered != 0U && bypass_n != 0U) ? 1U : 0U;
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  pdg_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra_isr_globals_enable();

  if (pdg_demo_configure() != k_ra_ok) {
    pdg_demo_panic_halt();
  }

  while (1) {
    uint8_t        ok   = 0U;
    const ra_err_t err  = pdg_demo_sample(&ok);
    const uint8_t  good = (err == k_ra_ok && ok != 0U) ? 1U : 0U;
    g_pdg_cfg_ok        = (uint32_t)good;
    if (good != 0U) {
      (void)ra_board_uart_console_write(k_pdg_demo_ok_msg,
                                        (size_t)(sizeof(k_pdg_demo_ok_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_board_uart_console_write(k_pdg_demo_bad_msg,
                                        (size_t)(sizeof(k_pdg_demo_bad_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ++g_pdg_heartbeat;
    ra_delay_ms(k_pdg_demo_period_ms);
  }
  pdg_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
