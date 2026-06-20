/**
 * @file examples/ek_ra8d2/lvd_monitor_demo/main.c
 * @brief Safe LVD / PVD VCC-monitor demo for the EK-RA8D2
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The Programmable Voltage Detection (PVD, a.k.a. LVD) block continuously
 * compares VCC against a programmable threshold Vdetm and exposes the
 * result through PVD1SR.MON (current level) and PVD1SR.DET (a latched
 * crossing). This demo configures voltage monitor 1 (PVD1) in the
 * *safest possible* mode -- ``k_ra_lvd_response_none``: flags only, with
 * **no** reset, NMI, or maskable interrupt wired up -- so observing a
 * brown-out can never reset or brick the board. The threshold is set low
 * (2.80 V) so a healthy 3.3 V rail reads MON = above, DET = 0.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs. PVD1 is configured once (the PVD
 * control registers sit behind ``PRCR.PRC3``, so the demo unlocks that
 * protection group around ``ra_lvd_channel_init`` and re-locks it after).
 * Once a second the loop reads PVD1SR and reports
 * ``"lvd: pvd1 thr=2.80V mon=above det=0 ok=Y\r\n"`` on the J-Link OB CDC
 * channel.
 *
 * LED1 toggles on every healthy read; LED2 toggles when VCC is reported
 * below threshold or a crossing is latched. The ``g_lvd_*`` globals
 * mirror the result for headless probing (HIL / board emulator).
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Headless-emulator status.** ``tools/board_sim`` shadows the PVD
 * control window but does not model the analog voltage comparator, so
 * PVD1SR.MON reads back 0 and the banner reports ``mon=below ok=N`` on the
 * emulator. This app therefore lives in ``hw_pending/`` until it is
 * confirmed on silicon (where a healthy rail yields ``mon=above ok=Y``).
 * See ``README.md`` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8d2_system_regs.h"
#include "ra_board_ek_ra8d2.h"
#include "ra_cgc.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_isr.h"
#include "ra_lvd.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_sci.h"
#include "ra_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "lvd_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_lvd_demo_baud        = 115200U, /**< SCI8 baud rate.                  */
  k_lvd_demo_period_ms   = 1000U,   /**< Delay between monitor reads.     */
  k_lvd_demo_sci_channel = 8U,      /**< J-Link OB CDC is on SCI8.        */
  k_lvd_demo_stab_ms     = 1U,      /**< t_d(E-A) settle before 1st read. */
} lvd_demo_config_t;

/**
 * @brief PRCR write values to (un)lock the PVD protection group.
 *
 * @details
 * The PVD control registers (PVD1CMPCR / PVD1CR0 / PVD1CR1 / PVD1SR /
 * PVD1FCR) are write-protected by ``PRCR.PRC3``. Each PVD register page
 * carries the note "Set the PRCR.PRC3 bit to 1 (write enabled) before
 * rewriting this register" (HUM Ch 8.2.2 p 303, Ch 8.2.4 p 305, Ch 8.2.6
 * p 307, Ch 8.2.7 p 307). PRCR is R_SYSTEM + 0x3FA; bit 15:8 is the
 * mandatory 0xA5 key. ``k_ra_prcr_grp2_osc`` (0x0008) is the PRC3 group
 * bit; OR it with the key to enable writes, write the key alone to relock.
 */
typedef enum : uint16_t {
  k_lvd_demo_prcr_unlock_pvd = (uint16_t)(k_ra_prcr_key | k_ra_prcr_grp2_osc), /**< 0xA508. */
} lvd_demo_prcr_t;

/** @brief Pinout for SCI8 on the J-Link OB CDC channel (PD02 / PD03). */
static const ra_port_pin_t k_lvd_demo_pin_txd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_2);
static const ra_port_pin_t k_lvd_demo_pin_rxd =
  (ra_port_pin_t)(((uint16_t)k_ra_port_13 << 8) | (uint16_t)k_ra_pin_3);

/** @brief Output line tags (2.80 V == k_ra_lvd_pvdlvl_2_80v). */
static const uint8_t k_lvd_demo_ok_msg[]  = "lvd: pvd1 thr=2.80V mon=above det=0 ok=Y\r\n";
static const uint8_t k_lvd_demo_bad_msg[] = "lvd: pvd1 thr=2.80V mon=below ok=N\r\n";

/**
 * @var g_lvd_ok
 * @brief 1 when the last read showed VCC above Vdetm with no latched crossing.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_lvd_ok = 0U;

/**
 * @var g_lvd_mon_above
 * @brief Last PVD1SR.MON value (1 = VCC above the 2.80 V threshold).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_lvd_mon_above = 0U;

/**
 * @var g_lvd_det
 * @brief Last PVD1SR.DET value (1 = a Vdetm crossing was latched).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_lvd_det = 0U;

/**
 * @var g_lvd_cfg_err
 * @brief Non-zero if ``ra_lvd_channel_init`` rejected the configuration.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_lvd_cfg_err = 0U;

/**
 * @var g_lvd_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_lvd_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void lvd_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Route PD02 / PD03 to SCI8 TXD/RXD via PFS.
 *
 * @return ``ra_err_t`` error code from the underlying PFS routing.
 * @pre IOPORT module reachable.
 * @post On success PD02 + PD03 are in SCI-async mode.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lvd_demo_pins_init(void)
{
  ra_err_t err = ra_pfs_route_peripheral(k_lvd_demo_pin_txd, k_ra_psel_sci_async, "lvd_demo.txd8");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_pfs_route_peripheral(k_lvd_demo_pin_rxd, k_ra_psel_sci_async, "lvd_demo.rxd8");
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void lvd_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;
  uint32_t pclka_hz   = 0U;

  if (ra_cgc_init() != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_cpuclk0, &cpuclk0_hz) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_cgc_get_clock_hz(k_ra_clock_id_pclka, &pclka_hz) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_mstp_init() != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_time_init(cpuclk0_hz) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (lvd_demo_pins_init() != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  const ra_sci_cfg_t sci_cfg = {
    .baud      = k_lvd_demo_baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = pclka_hz,
  };
  if (ra_sci_init((uint8_t)k_lvd_demo_sci_channel, &sci_cfg) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led1) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  if (ra_board_led_init(k_ra_board_led2) != k_ra_ok) {
    lvd_demo_panic_halt();
  }
}

/**
 * @brief Configure PVD1 as a flags-only 2.80 V monitor (no reset / NMI).
 *
 * @details
 * Unlocks ``PRCR.PRC3`` (the PVD protection group), runs the HUM Table 8.4
 * setup through ``ra_lvd_channel_init`` with ``response = none`` so neither
 * a reset nor an interrupt is ever armed, then re-locks PRCR. The digital
 * filter is left off to avoid a LOCO dependency. ``g_lvd_cfg_err`` latches
 * a non-zero code if the descriptor is rejected.
 *
 * @par MC/DC:
 * Single decision ``err != k_ra_ok`` -- 2 vectors (accept on silicon /
 * emulator where args are valid; reject is covered by the host test's
 * bad-arg path). No compound condition.
 *
 * @return ``ra_err_t`` -- ``k_ra_ok`` or the init error code.
 * @pre CGC is up; IRQs masked (single-threaded init).
 * @post PRCR is re-locked on every return path.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lvd_demo_configure(void)
{
  const ra_lvd_cfg_t cfg = {
    .threshold    = k_ra_lvd_pvdlvl_2_80v,
    .edge         = k_ra_lvd_edge_both,
    .irq_type     = k_ra_lvd_irq_maskable,
    .response     = k_ra_lvd_response_none, /* SAFE: flags only, no reset/NMI. */
    .negate       = k_ra_lvd_negate_after_voltage,
    .hysteresis   = k_ra_lvd_hysteresis_lvd,
    .filter_div   = k_ra_lvd_loco_div_8,
    .filter_en    = false,
    .irq_enable   = false,
    .clear_status = true,
  };

  /* HUM Ch 8.2.4 "PVDmCR0" p 305: "Set the PRCR.PRC3 bit to 1 (write
   * enabled) before rewriting this register." PRCR = R_SYSTEM + 0x3FA,
   * key 0xA5 in [15:8], PRC3 = bit 3. */
  *ra_sys_prcr()     = (uint16_t)k_lvd_demo_prcr_unlock_pvd;
  const ra_err_t err = ra_lvd_channel_init(k_ra_lvd_ch1, &cfg);
  ra_sys_prcr_lock_all();

  g_lvd_cfg_err = (err == k_ra_ok) ? 0U : (uint32_t)err;
  return err;
}

/**
 * @brief Read PVD1SR once and fold it into the "healthy rail" verdict.
 *
 * @details
 * A read is "ok" only when VCC is above the threshold (``st.above``) and
 * no crossing is currently latched (``!st.crossed``).
 *
 * @param[out] out_ok 1 when the rail is healthy, else 0.
 *
 * @par MC/DC:
 * Decision ``ok = st.above && !st.crossed`` (2 conditions). The host test
 * supplies N+1 = 3 vectors: (above=1,crossed=0)->1 control,
 * (above=0,crossed=0)->0 varies ``above``, (above=1,crossed=1)->0 varies
 * ``crossed``.
 *
 * @return ``ra_err_t`` from ``ra_lvd_get_status``.
 * @retval k_ra_err_null_ptr ``out_ok`` was NULL.
 * @pre PVD1 has been configured by ::lvd_demo_configure.
 * @post ``*out_ok`` is 0 or 1; ``g_lvd_mon_above`` / ``g_lvd_det`` updated.
 * @since 0.1.0
 */
[[nodiscard]] static ra_err_t lvd_demo_sample(uint8_t* out_ok)
{
  RA_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  ra_lvd_status_t st  = {.crossed = false, .above = false};
  const ra_err_t  err = ra_lvd_get_status(k_ra_lvd_ch1, &st);
  if (err != k_ra_ok) {
    return err;
  }
  g_lvd_mon_above = st.above ? 1U : 0U;
  g_lvd_det       = st.crossed ? 1U : 0U;
  *out_ok         = (st.above && !st.crossed) ? 1U : 0U;
  return k_ra_ok;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  lvd_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch: ra_delay_ms() uses the
   * SysTick tick path only when IRQs are globally enabled, otherwise it
   * busy-waits on DWT_CYCCNT. No NVIC sources are armed (response=none). */
  ra_isr_globals_enable();
  if (lvd_demo_configure() != k_ra_ok) {
    lvd_demo_panic_halt();
  }
  ra_delay_ms((uint32_t)k_lvd_demo_stab_ms);

  while (1) {
    uint8_t        ok   = 0U;
    const ra_err_t err  = lvd_demo_sample(&ok);
    const uint8_t  good = (err == k_ra_ok && ok != 0U) ? 1U : 0U;
    g_lvd_ok            = (uint32_t)good;
    if (good != 0U) {
      (void)ra_sci_write_polling((uint8_t)k_lvd_demo_sci_channel,
                                 k_lvd_demo_ok_msg,
                                 (uint32_t)(sizeof(k_lvd_demo_ok_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led1);
    } else {
      (void)ra_sci_write_polling((uint8_t)k_lvd_demo_sci_channel,
                                 k_lvd_demo_bad_msg,
                                 (uint32_t)(sizeof(k_lvd_demo_bad_msg) - 1U));
      (void)ra_board_led_toggle(k_ra_board_led2);
    }
    ++g_lvd_heartbeat;
    ra_delay_ms(k_lvd_demo_period_ms);
  }
  lvd_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
