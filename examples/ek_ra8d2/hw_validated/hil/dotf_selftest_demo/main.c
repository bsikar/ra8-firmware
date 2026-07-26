/**
 * @file examples/ek_ra8d2/hw_validated/hil/dotf_selftest_demo/main.c
 * @brief DOTF (Decryption On The Fly) bring-up + AES self-test demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The RA8D2 DOTF block (HUM Ch 45 p 3048..3050) transparently decrypts read
 * traffic on the AXI side of the OSPI / xSPI controller using an AES core in
 * CTR mode, so encrypted code in external flash can execute in place. The
 * block also exposes a built-in AES **self-test** (HUM Ch 45.1 p 3048,
 * "Supports self-test function"), triggered by REG00 bit 20.
 *
 * This demo exercises the **safe, key-free** half of the driver only:
 *
 *  1. ``ra8_dotf_init`` -- clock-ungate both channels (MSTPB16/17, shared with
 *     OSPI0/1) and reset REG00 to its bypass value. Neither channel is armed.
 *  2. ``ra8_dotf_run_self_test`` on channels 0 and 1 -- set REG00 bit 20, poll
 *     for it to clear (bounded), and snapshot REG00. The host/sim result is
 *     opaque diagnostic data (the AES pass/fail timing only exists on
 *     silicon), so the verdict gates on the calls *succeeding*, not on the
 *     snapshot value.
 *  3. ``ra8_dotf_get_status`` -- read REG00 back for the bench probe.
 *
 * **What this demo deliberately does NOT do:** install a key, stage an IV,
 * program a conversion region, or ``ra8_dotf_enable`` a channel. Arming the
 * AES core over a live XiP window would fault the next instruction fetch
 * (HUM Ch 45 warning), and key handling needs an ra8_rsip-wrapped key. None
 * of that is touched here -- both channels stay in transparent bypass, so the
 * demo writes **no** keys, IV, region, OTP, option-setting, or flash. It is a
 * read-mostly bring-up probe with no persistent side effects.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs + MSTP. Once a second:
 * ``"dotf: ch0/1 init=ok selftest=run ok=Y\r\n"``. The raw REG00 snapshots go
 * to ``g_dotf_reg00`` / ``g_dotf_st0_snap`` / ``g_dotf_st1_snap`` for
 * on-silicon probing. LED1 toggles while healthy; LED2 toggles on a fault.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Headless-emulator status.** ``tools/ra8_emulator`` does not model the
 * DOTF APB window (``0x4026_8800`` / ``0x4026_8900``), so REG00 reads are not
 * a real AES core -- the bring-up + self-test calls return ``k_ra8_ok`` and
 * the demo reports ``ok=Y``, but the genuine AES BIST result + the in-place
 * decryption path only exist on silicon. The demo therefore lives in
 * ``hw_pending/``. See ``README.md`` for the bench plan.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_dotf.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "dotf_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_dotf_demo_baud      = 115200U, /**< Console baud rate.        */
  k_dotf_demo_period_ms = 1000U,   /**< Delay between self-tests. */
} dotf_demo_config_t;

/** @brief DOTF channels exercised by the demo. */
typedef enum : uint8_t {
  k_dotf_demo_ch0 = 0U, /**< DOTF0 (paired with XSPI0). */
  k_dotf_demo_ch1 = 1U, /**< DOTF1 (paired with XSPI1). */
} dotf_demo_chan_t;

/** @brief Sentinel for "ra8_dotf_init has not run yet" in g_dotf_init_err. */
typedef enum : uint32_t {
  k_dotf_demo_err_unset = 0xFFFFFFFFU, /**< Distinct from any ra8_err_t code. */
} dotf_demo_sentinel_t;

/** @brief Output line tags. */
static const uint8_t k_dotf_demo_ok_msg[]  = "dotf: ch0/1 init=ok selftest=run ok=Y\r\n";
static const uint8_t k_dotf_demo_bad_msg[] = "dotf: selftest=FAIL ok=N\r\n";

/**
 * @var g_dotf_ok
 * @brief 1 when both channels' self-test + status calls returned k_ra8_ok.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_dotf_ok = 0U;

/**
 * @var g_dotf_init_err
 * @brief ra8_err_t from ra8_dotf_init (0 == k_ra8_ok).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dotf_init_err = (uint32_t)k_dotf_demo_err_unset;

/**
 * @var g_dotf_reg00
 * @brief Last REG00 snapshot from ra8_dotf_get_status(ch0).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dotf_reg00 = 0U;

/**
 * @var g_dotf_st0_snap
 * @brief Channel-0 self-test REG00 snapshot (opaque on host/sim).
 * @note Read externally only. Meaningful on silicon (real AES BIST).
 * @since 0.1.0
 */
volatile uint32_t g_dotf_st0_snap = 0U;

/**
 * @var g_dotf_st1_snap
 * @brief Channel-1 self-test REG00 snapshot (opaque on host/sim).
 * @note Read externally only. Meaningful on silicon (real AES BIST).
 * @since 0.1.0
 */
volatile uint32_t g_dotf_st1_snap = 0U;

/**
 * @var g_dotf_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_dotf_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void dotf_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void dotf_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_dotf_demo_baud) != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    dotf_demo_panic_halt();
  }
}

/**
 * @brief Verdict: both channels' self-test + status calls succeeded.
 *
 * @param[in] st0_err     ra8_err_t from ra8_dotf_run_self_test(ch0).
 * @param[in] st1_err     ra8_err_t from ra8_dotf_run_self_test(ch1).
 * @param[in] status_err  ra8_err_t from ra8_dotf_get_status(ch0).
 *
 * @return 1 when every call returned k_ra8_ok, else 0.
 *
 * @par MC/DC:
 * Decision ``(st0_err == ok) && (st1_err == ok) && (status_err == ok)``
 * (3 conditions). N+1 = 4 vectors host-tested: all-ok -> 1; each error in
 * turn -> 0 (proves each condition independently flips the outcome).
 *
 * @pre All three arguments are ra8_err_t-domain values.
 * @post Returns a 0/1 verdict (no side effects).
 * @since 0.1.0
 */
[[nodiscard]] static uint8_t
dotf_demo_verdict(ra8_err_t st0_err, ra8_err_t st1_err, ra8_err_t status_err)
{
  const bool ok = (st0_err == k_ra8_ok) && (st1_err == k_ra8_ok) && (status_err == k_ra8_ok);
  return ok ? 1U : 0U;
}

/**
 * @brief Self-test both channels and read REG00 back; fold into the verdict.
 *
 * @param[out] out_ok 1 when both self-test calls AND the status read returned
 *                    k_ra8_ok. The opaque REG00 snapshots are reported via
 *                    globals, not gated (board_sim does not model the AES core).
 *
 * @return ``ra8_err_t`` -- the first non-ok status from the three sub-calls, or
 *         k_ra8_ok when all succeeded.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 *
 * @par MC/DC:
 * The compound verdict is delegated to ::dotf_demo_verdict (host-tested,
 * 4 vectors). This wrapper is a sequence of guarded calls.
 *
 * @pre ::ra8_dotf_init succeeded.
 * @post ``g_dotf_st0_snap`` / ``g_dotf_st1_snap`` / ``g_dotf_reg00`` updated.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t dotf_demo_sample(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  uint32_t        st0     = 0U;
  const ra8_err_t st0_err = ra8_dotf_run_self_test((uint8_t)k_dotf_demo_ch0, &st0);
  g_dotf_st0_snap         = st0;

  uint32_t        st1     = 0U;
  const ra8_err_t st1_err = ra8_dotf_run_self_test((uint8_t)k_dotf_demo_ch1, &st1);
  g_dotf_st1_snap         = st1;

  uint32_t        reg00    = 0U;
  const ra8_err_t stat_err = ra8_dotf_get_status((uint8_t)k_dotf_demo_ch0, &reg00);
  g_dotf_reg00             = reg00;

  *out_ok = dotf_demo_verdict(st0_err, st1_err, stat_err);
  if (st0_err != k_ra8_ok) {
    return st0_err;
  }
  if (st1_err != k_ra8_ok) {
    return st1_err;
  }
  return stat_err;
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  dotf_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No maskable NVIC
   * source is armed; DOTF raises no IRQ of its own. */
  ra8_isr_globals_enable();

  const ra8_err_t init_err = ra8_dotf_init();
  g_dotf_init_err          = (uint32_t)init_err;
  if (init_err != k_ra8_ok) {
    dotf_demo_panic_halt();
  }

  while (1) {
    uint8_t         ok   = 0U;
    const ra8_err_t err  = dotf_demo_sample(&ok);
    const uint8_t   good = (err == k_ra8_ok && ok != 0U) ? 1U : 0U;
    g_dotf_ok            = (uint32_t)good;
    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_dotf_demo_ok_msg,
                                         (size_t)(sizeof(k_dotf_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_dotf_demo_bad_msg,
                                         (size_t)(sizeof(k_dotf_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    ++g_dotf_heartbeat;
    ra8_delay_ms(k_dotf_demo_period_ms);
  }
  dotf_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
