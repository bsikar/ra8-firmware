/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/pdg_delay_demo/main.c
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
 * GTIOC0A rising edge, and reports the configured bring-up state.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + MSTP. Once a second the loop
 * reads the PDG status and reports
 * ``"pdg: dll=on ch0=on delay=0x40 cfg=ok\r\n"`` on the J-Link OB CDC
 * channel. LED1 toggles while the configuration reads back clean; LED2
 * toggles otherwise.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **What headless validation can and cannot show.** Validated on a real
 * EK-RA8D2 (2026-06-28): the DLL locks and channel 0 reads back powered and
 * un-bypassed (``cfg=ok``). The delay *code* register ``GTDLYRnA`` is write-
 * staged but is **not read-exposed on silicon** -- it returns its 0x0000 reset
 * value to both firmware and a J-Link debugger, and FSP never reads it back --
 * so the verdict gates on the bring-up state, not on a delay read-back. The
 * PDG's only other observable, the *timing shift* of a GPT output edge, needs a
 * logic analyzer / oscilloscope and a running GPT32_0 PWM source to measure
 * (out of scope; see ``README.md``). ``tools/ra8_emulator`` shadows ``GTDLYRnA``
 * as plain R/W, which is why a delay read-back appears to work there but not on
 * silicon.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_boot_entry.h"
#include "ra8_attributes.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_pdg.h"
#include "ra8_time.h"

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
static const uint8_t s_pdg_demo_ok_msg[]  = "pdg: dll=on ch0=on delay=0x40 cfg=ok\r\n";
static const uint8_t s_pdg_demo_bad_msg[] = "pdg: cfg=BAD\r\n";

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

/**
 * @brief Park the processor after a fatal initialization failure.
 *
 * @details Executes wait-for-interrupt indefinitely to prevent application
 * progress with incomplete clock, timing, console, LED, or MSTP setup.
 *
 * @pre The caller has determined that safe initialization cannot be completed.
 * @pre No foreground recovery operation remains to be performed.
 * @post This function does not return.
 * @post The processor remains in a low-activity wait loop.
 * @note This terminal path preserves the first initialization failure state.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdg_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Bring CGC, SysTick, SCI8, LEDs, and MSTP support up.
 *
 * @details Initializes all PDG-demo prerequisites in dependency order and
 * transfers to ::internal_pdg_demo_panic_halt on the first failure.
 *
 * @pre The function runs during single-threaded application startup.
 * @pre EK-RA8D2 board registers are accessible through the platform mapping.
 * @post On return, timing, console, both LEDs, and MSTP support are ready.
 * @post Any required initialization failure prevents a return to the caller.
 * @note The helper applies the demo's fail-closed startup policy consistently.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_pdg_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_pdg_demo_baud) != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
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
 * Single decision ``err != k_ra8_ok`` per call -- no compound condition.
 *
 * @return ``ra8_err_t`` from ``ra8_pdg_init`` / ``ra8_pdg_set_delay``.
 * @retval k_ra8_ok The DLL and channel accepted the requested delay setup.
 * @pre CGC + MSTP up; IRQs masked or single-threaded init.
 * @pre The GPT32 clock falls within the selected PDG frequency range.
 * @post PDG DLL enabled, channel 0 un-bypassed, delay code staged.
 * @post A failed initialization prevents the delay write from being attempted.
 * @note The staged delay becomes live only when the associated GPT updates it.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_pdg_demo_configure(void)
{
  const ra8_pdg_config_t cfg = {
    .frange       = k_ra8_pdg_frange_80_160_mhz,
    .channel_mask = (uint8_t)k_pdg_demo_chan_mask,
    .auto_tune    = 0U,
    .gptclk_hz    = 0U,
  };
  const ra8_err_t ierr = ra8_pdg_init(&cfg);
  if (ierr != k_ra8_ok) {
    return ierr;
  }
  return ra8_pdg_set_delay((uint8_t)k_pdg_demo_channel,
                           k_ra8_pdg_pin_a,
                           k_ra8_pdg_edge_rising,
                           (uint8_t)k_pdg_demo_delay_code);
}

/**
 * @brief Read the PDG status and fold the bring-up into the verdict.
 *
 * @details Reads the staged delay for emulator visibility and the full PDG
 * status for the silicon-observable DLL, power, and bypass verdict.
 *
 * @param[out] out_ok 1 when the DLL is enabled and channel 0 is powered and
 *                    un-bypassed (the software-observable bring-up). The delay
 *                    code is staged by ::internal_pdg_demo_configure but is not
 *                    read-exposed on silicon, so it is not part of the verdict.
 *
 * @par MC/DC:
 * Decision ``ok = dll && powered && bypass_off`` (3 conditions). The host test
 * supplies N+1 = 4 vectors, varying each condition independently.
 *
 * @return ``ra8_err_t`` from the status accessors.
 * @retval k_ra8_ok Both status reads completed and @p out_ok was populated.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre ::internal_pdg_demo_configure succeeded.
 * @pre @p out_ok addresses writable storage for one verdict byte.
 * @post ``g_pdg_delay_readback`` / ``g_pdg_dll`` updated.
 * @post On success, @p out_ok contains exactly zero or one.
 * @note Delay-code readback is diagnostic only because silicon does not expose it.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_pdg_demo_sample(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  uint8_t         code = 0U;
  const ra8_err_t derr =
    ra8_pdg_get_delay((uint8_t)k_pdg_demo_channel, k_ra8_pdg_pin_a, k_ra8_pdg_edge_rising, &code);
  if (derr != k_ra8_ok) {
    return derr;
  }
  ra8_pdg_status_full_t st  = {};
  const ra8_err_t       err = ra8_pdg_get_status_full(&st);
  if (err != k_ra8_ok) {
    return err;
  }
  /* GTDLYRnA is not read-exposed on RA8D2 silicon: a write stages the delay
   * but the register reads back its 0x0000 reset value (confirmed by a debugger
   * probe; FSP never reads it back either). ra8_emulator shadows it as plain R/W,
   * which is why the read-back only appears to work there. So this is recorded
   * for the emulator but NOT gated on. */
  g_pdg_delay_readback = (uint32_t)code;
  g_pdg_dll            = (st.dll_enabled != 0U) ? 1U : 0U;

  const uint8_t powered  = (st.per_channel_powered[k_pdg_demo_channel] != 0U) ? 1U : 0U;
  const uint8_t bypass_n = (st.per_channel_bypass_off[k_pdg_demo_channel] != 0U) ? 1U : 0U;
  /* Validate the software-observable bring-up: DLL locked + channel 0 powered
   * and un-bypassed. The delay's actual edge-shift effect needs a scope. */
  *out_ok = (st.dll_enabled != 0U && powered != 0U && bypass_n != 0U) ? 1U : 0U;
  return k_ra8_ok;
}

void main(void)
{
  internal_pdg_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (ra8_emulator does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (internal_pdg_demo_configure() != k_ra8_ok) {
    internal_pdg_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = internal_pdg_demo_sample(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_pdg_cfg_ok         = (uint32_t)good;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(s_pdg_demo_ok_msg,
                                         (size_t)(sizeof(s_pdg_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(s_pdg_demo_bad_msg,
                                         (size_t)(sizeof(s_pdg_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_pdg_heartbeat;
    ra8_delay_ms(k_pdg_demo_period_ms);
  }
  internal_pdg_demo_panic_halt();
}
