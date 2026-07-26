/**
 * @file examples/ek_ra8d2/hil_needs_revalidation/bkup_survival_demo/main.c
 * @brief VBATT backup-register read/write + reset-survival demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The RA8D2 keeps 128 bytes (32 x 32-bit ``VBTBKRn`` words) of backup
 * register storage in the VBATT-backed power domain, which retains its
 * contents across a CPU reset (and, with a battery on VBATT, across a
 * power cycle). This demo exercises both halves of that contract:
 *
 *   1. **Read / write window.** Words 0..29 are filled with a
 *      deterministic pattern, read back, and compared byte-exact
 *      (``g_bkup_rw_ok``).
 *   2. **Reset survival.** Word 30 holds a sentinel and word 31 a boot
 *      counter. On the first boot the sentinel is absent, so it is
 *      written and the counter set to 1 (``g_bkup_survived = 0``). After
 *      a reset the sentinel is found intact, the counter is incremented,
 *      and ``g_bkup_survived = 1`` -- proving the domain retained state.
 *
 * Bring-up: CGC + SysTick + SCI8 + LEDs. Once a second the loop reports
 * ``"bkup: rw=ok survived=Y boot=3\r\n"`` on the J-Link OB CDC channel.
 * LED1 toggles while the read/write window is healthy; LED2 toggles if it
 * ever mismatches.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Silicon status (issue #131).** The dominant precondition for a live
 * VBTBKRn window is that voltage monitor 0 (LVD0) reset is enabled via the
 * ``OFS1.PVDAS`` option byte (HUM Ch 12.1.3 p 499, Ch 12.3.2 p 514); this app
 * sets ``OFS1 = 0xFFFFFFF0`` in ``CMakeLists.txt``. Bench debugger reads with
 * the default ``OFS1 = 0xFFFFFFFF`` (LVD0 off) show the whole VBATT area held
 * in ``VBATT_POR`` reset (``VBPORF = 1``) with every write dropped -- ``rw=BAD``.
 * The current HIL flash path (``scripts/hil/flash.sh``) strips the ``.option_
 * setting_*`` sections, so the ``OFS1`` change does not reach silicon through
 * it; on-silicon ``rw=ok`` needs a full-image / option-byte flash (see README).
 *
 * @note **Headless-emulator status.** ``tools/board_sim`` models the VBTBKRn
 * window as a reset-retained domain (``board_periph_bkup.c``) whose writes are
 * gated on ``VBTBER.VBAE`` (HUM Ch 12.2.6 p 504), so the read/write half passes
 * (``rw=ok``) only because this demo first arms VBAE via ``ra8_bkup_init`` -- a
 * firmware that forgot the VBAE step now reports ``rw=BAD`` on the sim too. The
 * emulator cannot model the ``OFS1``/LVD0 option-byte prerequisite (there is no
 * option memory in the sim), which is precisely why the sim reports ``rw=ok``
 * while the bench reports ``rw=BAD``. ``--reboot 1`` re-runs from the reset
 * vector with the retained domain, so the second boot finds the sentinel and
 * reports ``survived=Y`` (the ``board_sim_smoke.sh`` gate exercises this).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_bkup.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "bkup_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_bkup_demo_baud      = 115200U,     /**< SCI8 console baud rate.            */
  k_bkup_demo_period_ms = 1000U,       /**< Delay between reports.             */
  k_bkup_demo_sentinel  = 0x600DCAFEU, /**< "good cafe" reset-survival marker. */
  k_bkup_demo_pat_mult  = 0x01010101U, /**< Per-word pattern multiplier.       */
  k_bkup_demo_pat_xor   = 0xA5A5A5A5U, /**< Per-word pattern XOR mask.         */
} bkup_demo_config_t;

/** @brief Backup-word layout (32 words total; 0..29 data, 30/31 reserved). */
typedef enum : uint8_t {
  k_bkup_demo_data_words   = 30U, /**< Count of rw-pattern words (0..29).  */
  k_bkup_demo_idx_sentinel = 30U, /**< Reset-survival sentinel word index. */
  k_bkup_demo_idx_boot     = 31U, /**< Boot-counter word index.            */
} bkup_demo_layout_t;

/** @brief Output line prefixes. */
static const uint8_t k_bkup_demo_ok_msg[]  = "bkup: rw=ok ";
static const uint8_t k_bkup_demo_bad_msg[] = "bkup: rw=BAD ";
static const uint8_t k_bkup_demo_surv_y[]  = "survived=Y ";
static const uint8_t k_bkup_demo_surv_n[]  = "survived=N ";
static const uint8_t k_bkup_demo_crlf[]    = "\r\n";

/**
 * @var g_bkup_rw_ok
 * @brief 1 when words 0..29 read back exactly what was written.
 * @note Read externally only (HIL / board emulator).
 * @since 0.1.0
 */
volatile uint32_t g_bkup_rw_ok = 0U;

/**
 * @var g_bkup_survived
 * @brief 1 when the sentinel was found at boot (state retained across reset).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bkup_survived = 0U;

/**
 * @var g_bkup_boot_count
 * @brief Boot counter held in backup word 31 (increments per reset on HW).
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bkup_boot_count = 0U;

/**
 * @var g_bkup_heartbeat
 * @brief Bumps once per main-loop pass -- liveness for headless probes.
 * @note Read externally only.
 * @since 0.1.0
 */
volatile uint32_t g_bkup_heartbeat = 0U;

/** @brief Park forever after a fatal init failure. */
static void bkup_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/** @brief Deterministic pattern for backup word @p i. */
static uint32_t bkup_demo_pattern(uint8_t i)
{
  return ((uint32_t)i * (uint32_t)k_bkup_demo_pat_mult) ^ (uint32_t)k_bkup_demo_pat_xor;
}

/** @brief Bring CGC + SysTick + SCI8 + LEDs + MSTP up. */
static void bkup_demo_setup_or_halt(void)
{
  uint32_t cpuclk0_hz = 0U;

  if (ra8_cgc_init() != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_cgc_get_clock_hz(k_ra8_clock_id_cpuclk0, &cpuclk0_hz) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_mstp_init() != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_time_init(cpuclk0_hz) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_board_uart_console_init((uint32_t)k_bkup_demo_baud) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led1) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
  if (ra8_board_led_init(k_ra8_board_led2) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }

  /* Bring the VBATT backup-register block up before any VBTBKRn touch. Two
   * silicon preconditions must both hold or every VBTBKRn write is dropped:
   *
   *   1. Voltage monitor 0 (LVD0) reset must be enabled -- OFS1.PVDAS = 0
   *      (HUM Ch 12.1.3 p 499: "It is necessary to enable voltage monitor 0
   *      resets to use the battery backup function"; Ch 12.3.2 p 514). This
   *      is an option byte set in CMakeLists.txt (OFS1 = 0xFFFFFFF0); without
   *      it the VBATT area stays held in VBATT_POR reset (VBPORF = 1) and the
   *      whole block -- VBTBPCR1, VBTBKRn -- rejects writes. This is the
   *      dominant #131 root cause, verified on the bench by debugger reads.
   *   2. VBTBER.VBAE must be 1 before access -- HUM Ch 12.2.6 p 504 ("You
   *      must write 1 to VBAE before accessing VBTBKR", then "wait for at
   *      least 500 ns"). VBAE resets to 1, but ra8_bkup_init writes it and
   *      performs the mandatory settle so the sequence is explicit/correct.
   *
   * On the EK-RA8D2 VBATT is tied to VCC, so the battery power-supply switch
   * stays stopped (enable_switch = false). The window stays open for the life
   * of the demo (a J-Link/SYSRESETREQ reset keeps VCC/VBATT powered, so
   * leaving VBAE = 1 does not lose data). */
  const ra8_bkup_config_t bkup_cfg = {
    .vdet_level    = k_ra8_bkup_vdet_2p80v,
    .enable_switch = false,
    .enable_backup = true,
  };
  if (ra8_bkup_init(&bkup_cfg) != k_ra8_ok) {
    bkup_demo_panic_halt();
  }
}

/**
 * @brief Write words 0..29 with the pattern, read them back, and compare.
 *
 * @param[out] out_ok 1 if every word read back exactly, else 0.
 *
 * @par MC/DC:
 * Decision ``read != bkup_demo_pattern(i)`` (1 condition). Two vectors:
 * match (steady state) and mismatch (covered by the host test, which
 * seeds a differing read-back).
 *
 * @return ``ra8_err_t`` -- first accessor error, or ``k_ra8_ok``.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre Backup domain is powered (VBATT tied to VCC on the EVM).
 * @post ``*out_ok`` is 0 or 1; words 0..29 hold the pattern.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t bkup_demo_rw_check(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  for (uint8_t i = 0U; i < (uint8_t)k_bkup_demo_data_words; ++i) {
    const ra8_err_t werr = ra8_bkup_write_word(i, bkup_demo_pattern(i));
    if (werr != k_ra8_ok) {
      return werr;
    }
  }
  uint8_t ok = 1U;
  for (uint8_t i = 0U; i < (uint8_t)k_bkup_demo_data_words; ++i) {
    uint32_t        got  = 0U;
    const ra8_err_t rerr = ra8_bkup_read_word(i, &got);
    if (rerr != k_ra8_ok) {
      return rerr;
    }
    if (got != bkup_demo_pattern(i)) {
      ok = 0U;
    }
  }
  *out_ok = ok;
  return k_ra8_ok;
}

/**
 * @brief Detect reset survival and maintain the boot counter.
 *
 * @details
 * Reads the sentinel word. If it already holds the marker the domain
 * retained state across a reset, so the boot counter is incremented;
 * otherwise this is a cold boot, so the marker is planted and the counter
 * set to 1.
 *
 * @par MC/DC:
 * Decision ``sentinel == k_bkup_demo_sentinel`` (1 condition). Two
 * vectors: marker present (warm boot) and absent (cold boot), both
 * covered by the host test.
 *
 * @return ``ra8_err_t`` from the backup accessors.
 * @pre Backup domain is powered.
 * @post ``g_bkup_survived`` / ``g_bkup_boot_count`` reflect this boot; the
 *       sentinel + counter words are written.
 * @since 0.1.0
 */
[[nodiscard]] static ra8_err_t bkup_demo_survival_check(void)
{
  uint32_t        sentinel = 0U;
  const ra8_err_t serr     = ra8_bkup_read_word((uint8_t)k_bkup_demo_idx_sentinel, &sentinel);
  if (serr != k_ra8_ok) {
    return serr;
  }
  uint32_t boot = 0U;
  if (sentinel == (uint32_t)k_bkup_demo_sentinel) {
    g_bkup_survived      = 1U;
    const ra8_err_t rerr = ra8_bkup_read_word((uint8_t)k_bkup_demo_idx_boot, &boot);
    if (rerr != k_ra8_ok) {
      return rerr;
    }
    boot += 1U;
  } else {
    g_bkup_survived = 0U;
    boot            = 1U;
    const ra8_err_t merr =
      ra8_bkup_write_word((uint8_t)k_bkup_demo_idx_sentinel, (uint32_t)k_bkup_demo_sentinel);
    if (merr != k_ra8_ok) {
      return merr;
    }
  }
  const ra8_err_t cerr = ra8_bkup_write_word((uint8_t)k_bkup_demo_idx_boot, boot);
  if (cerr != k_ra8_ok) {
    return cerr;
  }
  g_bkup_boot_count = boot;
  return k_ra8_ok;
}

/** @brief Emit "survived=Y boot=N" suffix as ASCII over SCI8. */
static void bkup_demo_report_survival(void)
{
  const uint8_t* tag = (g_bkup_survived != 0U) ? k_bkup_demo_surv_y : k_bkup_demo_surv_n;
  /* Both suffixes are the same length ("survived=Y " / "survived=N "). */
  static_assert(sizeof(k_bkup_demo_surv_y) == sizeof(k_bkup_demo_surv_n),
                "survived=Y / survived=N suffixes must be equal length");
  const uint32_t len = (uint32_t)(sizeof(k_bkup_demo_surv_y) - 1U);
  (void)ra8_board_uart_console_write(tag, (size_t)len);
}

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmain"
int32_t main(void)
{
  bkup_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (board_sim does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (bkup_demo_survival_check() != k_ra8_ok) {
    bkup_demo_panic_halt();
  }

  while (1) {
    uint8_t         rw   = 0U;
    const ra8_err_t err  = bkup_demo_rw_check(&rw);
    const uint8_t   good = (err == k_ra8_ok && rw != 0U) ? 1U : 0U;
    g_bkup_rw_ok         = (uint32_t)good;

    if (good != 0U) {
      (void)ra8_board_uart_console_write(k_bkup_demo_ok_msg,
                                         (size_t)(sizeof(k_bkup_demo_ok_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led1);
    } else {
      (void)ra8_board_uart_console_write(k_bkup_demo_bad_msg,
                                         (size_t)(sizeof(k_bkup_demo_bad_msg) - 1U));
      (void)ra8_board_led_toggle(k_ra8_board_led2);
    }
    bkup_demo_report_survival();
    (void)ra8_board_uart_console_write(k_bkup_demo_crlf, (size_t)(sizeof(k_bkup_demo_crlf) - 1U));
    ++g_bkup_heartbeat;
    ra8_delay_ms(k_bkup_demo_period_ms);
  }
  bkup_demo_panic_halt();
  return 0;
}
#pragma GCC diagnostic pop
