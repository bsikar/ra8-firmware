/**
 * @file examples/ek_ra8d2/hw_validated/hil/bkup_survival_demo/src/main.c
 * @brief VBATT backup-register read/write + reset-survival demo (EK-RA8D2)
 *
 * @par Tag
 * [Ring 6 / APP] {World: S}
 *
 * @details
 * The RA8D2 keeps 128 bytes (32 x 32-bit ``VBTBKRn`` words) of backup
 * register storage in the VBATT-backed power domain, which retains its
 * contents across a CPU reset (and, with a battery on VBATT, across a
 * power cycle). This demo exercises both halves of that contract across two
 * self-orchestrated boots:
 *
 *   1. **Cold / non-software boot (boot=1).** Clears any stale sentinel,
 *      writes and verifies words 0..29 (``g_bkup_rw_ok``), plants the
 *      sentinel in word 30 and ``1`` in word 31 (``g_bkup_survived = 0``),
 *      prints ``bkup: rw=ok survived=N boot=1``, flushes UART, and issues
 *      ``ra8_reset_software_reset()``.
 *   2. **Software-reset boot (boot=2).** Observes reset cause
 *      ``k_ra8_reset_cause_software``, reads the retained sentinel intact,
 *      increments word 31 to ``2`` (``g_bkup_survived = 1``), and reports
 *      ``bkup: rw=ok survived=Y boot=2`` at 1 Hz while toggling LED1.
 *
 * Bare EK-RA8D2 only -- no shields or external transceivers.
 *
 * @note **Silicon & emulator behavior (RA8FW-254).** Register access to
 * ``VBT*`` requires SYSC ``PRCR.PRC1`` unlocked (HUM Ch 13.1 Table 13.1
 * p 520-521) and ``VBTBER.VBAE = 1`` with >= 500 ns settle (HUM Ch 12.2.6
 * p 504), both handled by ``ra8_bkup``. Both physical EK-RA8D2 silicon and
 * ``tools/ra8_emulator`` retain ``VBTBKRn`` across ``AIRCR.SYSRESETREQ``.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_bkup.h"
#include "ra8_board_ek_ra8d2.h"
#include "ra8_boot_entry.h"
#include "ra8_cgc.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_isr.h"
#include "ra8_mstp.h"
#include "ra8_reset.h"
#include "ra8_time.h"

/** @brief Diagnostic / log tag. */
static const char* s_tag = "bkup_demo";

/** @brief Compile-time settings. */
typedef enum : uint32_t {
  k_bkup_demo_baud            = 115200U,     /**< SCI8 console baud rate.            */
  k_bkup_demo_period_ms       = 1000U,       /**< Delay between reports.             */
  k_bkup_demo_reset_settle_ms = 100U,        /**< UART settle before SW reset.       */
  k_bkup_demo_dec_base        = 10U,         /**< Decimal radix for boot counter.    */
  k_bkup_demo_sentinel        = 0x600DCAFEU, /**< "good cafe" reset-survival marker. */
  k_bkup_demo_pat_mult        = 0x01010101U, /**< Per-word pattern multiplier.       */
  k_bkup_demo_pat_xor         = 0xA5A5A5A5U, /**< Per-word pattern XOR mask.         */
  k_bkup_demo_rstsr_all_mask  = 0x7FFFFFFFU, /**< Clear all RSTSR0/1 cause flags.    */
} bkup_demo_config_t;

/** @brief Backup-word layout (32 words total; 0..29 data, 30/31 reserved). */
typedef enum : uint8_t {
  k_bkup_demo_data_words   = 30U, /**< Count of rw-pattern words (0..29).  */
  k_bkup_demo_idx_sentinel = 30U, /**< Reset-survival sentinel word index. */
  k_bkup_demo_idx_boot     = 31U, /**< Boot-counter word index.            */
} bkup_demo_layout_t;

/**
 * @var s_bkup_demo_ok_msg
 * @brief Prefix for a successful backup-word read/write report.
 * @details Begins the periodic console line when all 30 pattern words match.
 * @note Followed by the reset-survival suffix.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_ok_msg[] = "bkup: rw=ok ";
/**
 * @var s_bkup_demo_bad_msg
 * @brief Prefix for a failed backup-word read/write report.
 * @details Identifies either an accessor error or a pattern mismatch.
 * @note Followed by the reset-survival suffix.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_bad_msg[] = "bkup: rw=BAD ";
/**
 * @var s_bkup_demo_surv_y
 * @brief Warm-reset survival suffix.
 * @details Reports that the retained sentinel was present at startup.
 * @note Kept the same byte length as the negative suffix.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_surv_y[] = "survived=Y ";
/**
 * @var s_bkup_demo_surv_n
 * @brief Cold-start survival suffix.
 * @details Reports that startup had to plant a new retained sentinel.
 * @note Kept the same byte length as the affirmative suffix.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_surv_n[] = "survived=N ";
/**
 * @var s_bkup_demo_boot_msg
 * @brief Prefix before the numeric boot counter in each report line.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_boot_msg[] = "boot=";
/**
 * @var s_bkup_demo_crlf
 * @brief Console line terminator for each periodic report.
 * @details Uses CRLF to match the board UART diagnostic convention.
 * @note The terminating null byte is excluded from writes.
 * @since 0.1.0
 */
static const uint8_t s_bkup_demo_crlf[] = "\r\n";

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

/**
 * @brief Park forever after a fatal initialization failure.
 * @details Repeatedly executes the target wait-for-interrupt instruction so
 *          the failed application cannot advance its heartbeat or diagnostics.
 * @pre A required platform or backup-domain initialization step failed.
 * @pre The caller accepts that control never returns.
 * @post The core remains in the permanent wait loop.
 * @post No success indicator is modified.
 * @note A debugger can inspect the call site to identify the failed dependency.
 * @since 0.1.0
 */
[[noreturn]] RA8_INTERNAL static void internal_bkup_demo_panic_halt(void)
{
  while (1) {
    __asm__ volatile("wfi");
  }
}

/**
 * @brief Derive the deterministic pattern for one backup word.
 * @details Multiplies the word index into repeated bytes and XORs the fixed
 *          mask, producing visibly distinct values for adjacent slots.
 * @param[in] i Backup data-word index.
 * @return Expected 32-bit pattern for ``i``.
 * @retval 0x00000000..0xFFFFFFFF Deterministic index-derived word.
 * @pre ``i`` is less than ``k_bkup_demo_data_words``.
 * @pre The compile-time multiplier and mask retain their documented values.
 * @post No backup register or global state is accessed.
 * @post Repeated calls with the same index return the same value.
 * @note The pattern is diagnostic, not cryptographic.
 * @since 0.1.0
 */
RA8_INTERNAL static uint32_t internal_bkup_demo_pattern(uint8_t i)
{
  return ((uint32_t)i * (uint32_t)k_bkup_demo_pat_mult) ^ (uint32_t)k_bkup_demo_pat_xor;
}

/**
 * @brief Initialize clocks, time, console, LEDs, and the backup domain.
 * @details Brings dependencies up in order, then explicitly enables VBATT
 *          backup access under the board's VCC-tied supply configuration.
 * @pre Core reset initialization has completed and peripheral MMIO is accessible.
 * @pre Configurable interrupts remain globally masked.
 * @post On success all periodic-report and retained-word dependencies are ready.
 * @post Any failed step transfers control to the permanent halt loop.
 * @note The backup access window intentionally remains enabled for this demo.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bkup_demo_setup_or_halt(void)
{
  const ra8_board_bringup_cfg_t bringup_cfg = {
    .console_baud = (uint32_t)k_bkup_demo_baud,
    .leds_mask    = (uint32_t)k_ra8_board_bringup_led1 | (uint32_t)k_ra8_board_bringup_led2,
  };
  ra8_board_bringup_out_t bringup_out = {};
  if (ra8_board_bringup(&bringup_cfg, &bringup_out) != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }

  /* Bring the VBATT backup-register block up before any VBTBKRn touch:
   * ``ra8_bkup`` unlocks SYSC ``PRCR.PRC1`` around every ``VBT*`` write
   * (HUM Ch 13.1 Table 13.1 p 520-521) and writes ``VBTBER.VBAE = 1`` with the
   * HUM Ch 12.2.6 p 504 >= 500 ns settle so the 128-byte store is writable.
   *
   * On the EK-RA8D2 VBATT is tied to VCC, so the battery power-supply switch
   * stays stopped (enable_switch = false). An AIRCR.SYSRESETREQ software reset
   * keeps VCC/VBATT powered, retaining VBTBKRn across the two boots. */
  const ra8_bkup_config_t bkup_cfg = {
    .vdet_level    = k_ra8_bkup_vdet_2p80v,
    .enable_switch = false,
    .enable_backup = true,
  };
  if (ra8_bkup_init(&bkup_cfg) != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }
}

/**
 * @brief Write words 0..29 with the pattern, read them back, and compare.
 *
 * @param[out] out_ok 1 if every word read back exactly, else 0.
 *
 * @par MC/DC:
 * Decision ``read != internal_bkup_demo_pattern(i)`` (1 condition). Two vectors:
 * match (steady state) and mismatch (covered by the host test, which
 * seeds a differing read-back).
 *
 * @return ``ra8_err_t`` -- first accessor error, or ``k_ra8_ok``.
 * @retval k_ra8_err_null_ptr ``out_ok`` was NULL.
 * @pre Backup domain is powered (VBATT tied to VCC on the EVM).
 * @pre Words 0..29 are available for this destructive pattern check.
 * @post ``*out_ok`` is 0 or 1; words 0..29 hold the pattern.
 * @post Reserved sentinel and boot-counter words remain unchanged.
 * @note Accessor errors stop the sweep immediately and leave ``*out_ok`` unspecified.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_bkup_demo_rw_check(uint8_t* out_ok)
{
  RA8_CHECK_NULL_PTR(out_ok, s_tag, "out_ok must not be nullptr");

  for (uint8_t i = 0U; i < (uint8_t)k_bkup_demo_data_words; ++i) {
    const ra8_err_t werr = ra8_bkup_write_word(i, internal_bkup_demo_pattern(i));
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
    if (got != internal_bkup_demo_pattern(i)) {
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
 * @retval k_ra8_ok Retained-state diagnostics and counter were updated.
 * @pre Backup domain is powered.
 * @pre Sentinel and boot-counter slots are reserved for this application.
 * @post ``g_bkup_survived`` / ``g_bkup_boot_count`` reflect this boot; the
 *       sentinel + counter words are written.
 * @post Pattern-test words 0..29 remain unchanged.
 * @note A cold boot plants the sentinel and starts the counter at one.
 * @since 0.1.0
 */
[[nodiscard]] RA8_INTERNAL static ra8_err_t internal_bkup_demo_survival_check(void)
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

/**
 * @brief Emit the fixed-width reset-survival suffix over the console.
 * @details Selects the affirmative or negative string from the retained-state
 *          diagnostic and writes its common payload length without allocation.
 * @pre The board UART console has been initialized.
 * @pre The two suffix arrays retain equal compile-time lengths.
 * @post Exactly one survival suffix has been offered to the console backend.
 * @post Retained-state globals remain unchanged.
 * @note The boot count is exposed through ``g_bkup_boot_count`` for SWD probes.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_bkup_demo_report_survival(void)
{
  const uint8_t* tag = (g_bkup_survived != 0U) ? s_bkup_demo_surv_y : s_bkup_demo_surv_n;
  /* Both suffixes are the same length ("survived=Y " / "survived=N "). */
  static_assert(sizeof(s_bkup_demo_surv_y) == sizeof(s_bkup_demo_surv_n),
                "survived=Y / survived=N suffixes must be equal length");
  const uint32_t len = (uint32_t)(sizeof(s_bkup_demo_surv_y) - 1U);
  (void)ra8_board_uart_console_write(tag, (size_t)len);
}

/**
 * @brief Write an unsigned 32-bit decimal counter to the UART console.
 * @param[in] value Counter value to format in base 10.
 */
RA8_INTERNAL static void internal_bkup_demo_put_u32(uint32_t value)
{
  uint8_t  digits[10] = {};
  uint32_t count      = 0U;
  uint32_t rem        = value;

  do {
    digits[count] = (uint8_t)('0' + (rem % (uint32_t)k_bkup_demo_dec_base));
    rem /= (uint32_t)k_bkup_demo_dec_base;
    ++count;
  } while ((rem != 0U) && (count < (uint32_t)sizeof(digits)));

  while (count > 0U) {
    --count;
    (void)ra8_board_uart_console_write(&digits[count], 1U);
  }
}

/**
 * @brief Emit one full status line: rw verdict, survival flag, and boot count.
 * @param[in] good True when backup pattern read-back matched word-for-word.
 */
RA8_INTERNAL static void internal_bkup_demo_emit_status(bool good)
{
  if (good) {
    (void)ra8_board_uart_console_write(s_bkup_demo_ok_msg,
                                       (size_t)(sizeof(s_bkup_demo_ok_msg) - 1U));
  } else {
    (void)ra8_board_uart_console_write(s_bkup_demo_bad_msg,
                                       (size_t)(sizeof(s_bkup_demo_bad_msg) - 1U));
  }
  internal_bkup_demo_report_survival();
  (void)ra8_board_uart_console_write(s_bkup_demo_boot_msg,
                                     (size_t)(sizeof(s_bkup_demo_boot_msg) - 1U));
  internal_bkup_demo_put_u32(g_bkup_boot_count);
  (void)ra8_board_uart_console_write(s_bkup_demo_crlf, (size_t)(sizeof(s_bkup_demo_crlf) - 1U));
}

/**
 * @brief Return true only when entry matches our Boot 1 -> Boot 2 self-reset.
 *
 * @param[in] rstsr1    Raw RSTSR1 flags captured on entry.
 * @param[in] sentinel  Backup sentinel read before planting/incrementing.
 * @param[in] prev_boot Previous boot counter read from backup storage.
 * @return True when SWRF is set right after Boot 1 planted sentinel + boot=1.
 */
[[nodiscard]] RA8_INTERNAL static bool
internal_bkup_demo_is_boot2(uint32_t rstsr1, uint32_t sentinel, uint32_t prev_boot)
{
  if ((rstsr1 & (uint32_t)k_ra8_reset_rstsr1_swrf_msk) == 0U) {
    return false;
  }
  return (sentinel == (uint32_t)k_bkup_demo_sentinel) && (prev_boot == 1U);
}

/**
 * @brief Run one rw check, emit Boot 1 status, and trigger software reset.
 * @note Never returns; calls ``ra8_reset_software_reset()`` when rw passed.
 */
[[noreturn]] RA8_INTERNAL static void internal_bkup_demo_cold_boot_reset_or_halt(void)
{
  uint8_t         rw   = 0U;
  const ra8_err_t err  = internal_bkup_demo_rw_check(&rw);
  const bool      good = (err == k_ra8_ok && rw != 0U);
  g_bkup_rw_ok         = good ? 1U : 0U;

  internal_bkup_demo_emit_status(good);
  (void)ra8_board_uart_console_flush();
  if (!good) {
    (void)ra8_bkup_write_word((uint8_t)k_bkup_demo_idx_sentinel, 0U);
    internal_bkup_demo_panic_halt();
  }
  ra8_delay_ms((uint32_t)k_bkup_demo_reset_settle_ms);
  ra8_reset_software_reset();
  internal_bkup_demo_panic_halt();
}

void main(void)
{
  internal_bkup_demo_setup_or_halt();
  /* Clear PRIMASK so SysTick can dispatch and ra8_delay_ms() uses the
   * SysTick path (ra8_emulator does not advance DWT_CYCCNT). No NVIC sources
   * are armed by this demo. */
  ra8_isr_globals_enable();

  if (ra8_reset_init() != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }
  ra8_reset_raw_t raw = {};
  if (ra8_reset_get_raw(&raw) != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }
  /* Sticky RSTSR0/1 flags (for example PORF or WDTRF from earlier runs)
   * can mask SWRF, while J-Link flash reset raises SWRF via SYSRESETREQ.
   * Clear all flags on entry and gate Boot 2 on raw SWRF + boot=1 state. */
  if (ra8_reset_clear_cause((uint32_t)k_bkup_demo_rstsr_all_mask) != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }

  uint32_t sentinel  = 0U;
  uint32_t prev_boot = 0U;
  if ((ra8_bkup_read_word((uint8_t)k_bkup_demo_idx_sentinel, &sentinel) != k_ra8_ok) ||
      (ra8_bkup_read_word((uint8_t)k_bkup_demo_idx_boot, &prev_boot) != k_ra8_ok)) {
    internal_bkup_demo_panic_halt();
  }
  const bool boot2 = internal_bkup_demo_is_boot2(raw.rstsr1, sentinel, prev_boot);

  if (!boot2) {
    /* Fresh flash, POR, or repeat HIL run (prev_boot >= 2): clear any
     * leftover sentinel so Boot 1 deterministically emits survived=N boot=1
     * before issuing ra8_reset_software_reset(). */
    if (ra8_bkup_write_word((uint8_t)k_bkup_demo_idx_sentinel, 0U) != k_ra8_ok) {
      internal_bkup_demo_panic_halt();
    }
  }

  if (internal_bkup_demo_survival_check() != k_ra8_ok) {
    internal_bkup_demo_panic_halt();
  }

  if (!boot2) {
    internal_bkup_demo_cold_boot_reset_or_halt();
  }

  while (1) {
    uint8_t         rw   = 0U;
    const ra8_err_t err  = internal_bkup_demo_rw_check(&rw);
    const bool      good = (err == k_ra8_ok && rw != 0U);
    g_bkup_rw_ok         = good ? 1U : 0U;

    internal_bkup_demo_emit_status(good);
    (void)ra8_board_led_toggle(good ? k_ra8_board_led1 : k_ra8_board_led2);
    ++g_bkup_heartbeat;
    ra8_delay_ms(k_bkup_demo_period_ms);
  }
  internal_bkup_demo_panic_halt();
}
