/**
 * @file ra8_wdt.c
 * @brief Software Watchdog Timer (WDT) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Register-start-mode driver for the RA8D2 WDT (HUM Ch 27,
 * p 1256-1270). The companion IWDT lives in ``ra8_iwdt.c``; do not
 * confuse the two.
 *
 * ## OFS0 relationship
 *
 * In auto-start mode the OFS0 option-setting register (HUM Ch 7)
 * latches every period / clock-divider / window / reset-vs-NMI /
 * Sleep-stop bit *before* this driver runs, and the runtime WDTCR /
 * WDTRCR / WDTCSTPR registers become read-only-as-zero. This driver
 * therefore makes ``ra8_wdt_init()``'s register writes harmless
 * no-ops in that mode while still issuing the first refresh -- so
 * the same call site works in either configuration. The decoded
 * OFSm view is exposed by ``ra8_wdt_ofs_get()`` so the application
 * can introspect what the boot ROM latched.
 *
 * In register-start mode (``OFS0.WDT0STRT = 1``) the same
 * ``ra8_wdt_init()`` writes WDTCR / WDTRCR / WDTCSTPR exactly once and
 * then refreshes WDTRR to arm the counter. HUM Ch 27.3.2 limits these
 * three control registers to a single post-reset write, which the
 * driver respects implicitly by exposing only one ``init`` entry
 * point.
 *
 * ## NMI wiring
 *
 * The WDT underflow / refresh-error event is *not* an IELSR-routed
 * peripheral interrupt -- it is a non-maskable interrupt source on
 * the ICU's NMIER (HUM Ch 14.2.14 p 542 lists ``WDTEN`` at bit 1).
 * ``ra8_wdt_install_nmi`` enables that bit (and clears any stale
 * status); the dispatch entry point ``ra8_wdt_dispatch`` is what the
 * NMI handler calls.
 *
 * ## Multi-subscriber model
 *
 * The driver maintains a static ``k_ra8_wdt_max_subs`` slot table so
 * several modules (state-of-health logger, crash recorder, app
 * cleanup task, ...) can fan-out the same event without any module
 * owning the single callback slot exclusively. The legacy single-
 * callback ``ra8_wdt_attach_handler`` still works -- it owns one
 * dedicated slot in the same table.
 *
 * Every register access carries a HUM Ch 27.x citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra8_wdt.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"
#include "ra8_wdt_regs.h"

/**
 * @var s_tag
 * @brief Module log tag.
 *
 * @details
 * Passed as the first argument to ``ra8_log_*`` so log lines are
 * easy to grep.
 *
 * @note Read-only; do not modify.
 */
static const char* const s_tag = "WDT";

/**
 * @enum ra8_wdt_status_combined_t
 * @brief Union of the two WDTSR top-flag bits the driver cares about.
 */
typedef enum : uint16_t {
  k_ra8_wdt_status_all =
    k_ra8_wdt_status_underflow | k_ra8_wdt_status_refresh, /**< RA8 wdt status all. */
} ra8_wdt_status_combined_t;

/* =============================================================================
 * Internal helpers
 * =============================================================================
 */

/**
 * @brief Reject CKS encodings the silicon marks as "Setting prohibited".
 *
 * @param[in] div Caller-supplied divider value.
 * @return ``true`` iff ``div`` is one of the legal encodings in HUM
 *         Ch 27.2.2 p 1258.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_clock_div_is_valid(ra8_wdt_clock_div_t div)
{
  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 -- legal
   * CKS[3:0] encodings are 0x1, 0x4, 0x6, 0x7, 0x8, 0xF only. */
  switch (div) {
    case k_ra8_wdt_clkdiv_4:
    case k_ra8_wdt_clkdiv_64:
    case k_ra8_wdt_clkdiv_128:
    case k_ra8_wdt_clkdiv_512:
    case k_ra8_wdt_clkdiv_2048:
    case k_ra8_wdt_clkdiv_8192:
      return true;
    default:
      return false;
  }
}

/**
 * @brief Reject TOPS encodings outside the documented 2-bit range.
 *
 * @param[in] sel Caller-supplied timeout selector.
 * @return ``true`` iff ``sel`` is one of the four legal TOPS values.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static bool internal_timeout_sel_is_valid(ra8_wdt_timeout_sel_t sel)
{
  /* HUM Ch 27.2.2 "WDTCR" p 1259 */
  bool ok = false;
  switch (sel) {
    case k_ra8_wdt_timeout_1024:
    case k_ra8_wdt_timeout_4096:
    case k_ra8_wdt_timeout_8192:
    case k_ra8_wdt_timeout_16384:
      ok = true;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Pack a ra8_wdt_cfg_t into a 16-bit WDTCR word.
 *
 * @param[in] cfg Caller-validated configuration block.
 * @return The 16-bit value to write into WDTCR.
 *
 * @details See implementation.
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL static uint16_t internal_pack_wdtcr(const ra8_wdt_cfg_t* cfg)
{
  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 */
  const uint16_t tops = (uint16_t)((uint16_t)cfg->timeout & k_ra8_wdt_mask_tops);
  const uint16_t cks  = (uint16_t)((uint16_t)cfg->clock_div & k_ra8_wdt_mask_cks);
  const uint16_t rpes = (uint16_t)((uint16_t)cfg->window_end & k_ra8_wdt_mask_rpes);
  const uint16_t rpss = (uint16_t)((uint16_t)cfg->window_start & k_ra8_wdt_mask_rpss);

  uint16_t word = 0U;
  word |= (uint16_t)(tops << k_ra8_wdt_shift_tops);
  word |= (uint16_t)(cks << k_ra8_wdt_shift_cks);
  word |= (uint16_t)(rpes << k_ra8_wdt_shift_rpes);
  word |= (uint16_t)(rpss << k_ra8_wdt_shift_rpss);
  return word;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_wdt_init(const ra8_wdt_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (!internal_clock_div_is_valid(cfg->clock_div)) {
    return k_ra8_err_invalid_arg;
  }
  if (!internal_timeout_sel_is_valid(cfg->timeout)) {
    return k_ra8_err_invalid_arg;
  }

  volatile r_wdt_regs_t* reg = ra8_wdt();

  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 -- one
   * 16-bit transaction commits the timeout / divider / window. */
  reg->WDTCR = internal_pack_wdtcr(cfg);

  /* HUM Ch 27.2.4 "WDTRCR : WDT Reset Control Register", p 1262 --
   * RSTIRQS selects internal reset (1) vs NMI / IRQ (0) on
   * underflow or refresh-error. */
  reg->WDTRCR =
    (uint8_t)((cfg->on_expiry == k_ra8_wdt_on_expiry_reset) ? k_ra8_wdt_rcr_rstirqs : 0U);

  /* HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register",
   * p 1262 -- SLCSTP halts the counter on Sleep / Deep Sleep. */
  reg->WDTCSTPR =
    (uint8_t)((cfg->stop_in_sleep == k_ra8_wdt_sleep_stop_count) ? k_ra8_wdt_cstpr_slcstp : 0U);

  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 -- the
   * 0x00 / 0xFF unlock sequence is what arms the down-counter in
   * register-start mode, and is harmless in auto-start mode. */
  ra8_wdt_refresh();

  ra8_log_info(s_tag, "wdt_init armed");
  return k_ra8_ok;
}

/* =============================================================================
 * Refresh
 * =============================================================================
 */

void ra8_wdt_refresh_deferred(void)
{
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  ra8_wdt_refresh();
}

[[nodiscard]] ra8_err_t ra8_wdt_refresh_for(ra8_wdt_instance_t which)
{
  if ((uint8_t)which >= k_ra8_wdt_instance_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  ra8_wdt_refresh_instance(which);
  return k_ra8_ok;
}

/* =============================================================================
 * Status read / clear / counter
 * =============================================================================
 */

[[nodiscard]] ra8_err_t ra8_wdt_get_status(uint16_t* out_mask)
{
  RA8_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 */
  *out_mask = (uint16_t)(ra8_wdt()->WDTSR & k_ra8_wdt_status_all);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_wdt_clear_status(void)
{
  volatile r_wdt_regs_t* reg = ra8_wdt();
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 -- UNDFF /
   * REFEF are write-0-to-clear; writing 1 has no effect. We mask
   * out the flag bits and write the result back. */
  reg->WDTSR = (uint16_t)(reg->WDTSR & (uint16_t)~k_ra8_wdt_status_all);
  return k_ra8_ok;
}

[[nodiscard]] ra8_err_t ra8_wdt_clear_status_blocking(uint16_t mask)
{
  if ((mask & ~k_ra8_wdt_status_all) != 0U) {
    return k_ra8_err_invalid_arg;
  }
  if (mask == k_ra8_wdt_status_none) {
    return k_ra8_ok;
  }

  volatile r_wdt_regs_t* reg = ra8_wdt();

  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1261 -- W0C
   * sequence: write the inverse of the bits we want to clear back
   * into WDTSR, then poll until the targeted bits read 0. The
   * (N + 1) PCLKB cycle latency is bounded by the divider; we
   * cap our polls at k_ra8_wdt_clear_max_polls (16384). */
  for (uint32_t poll = 0U; poll < k_ra8_wdt_clear_max_polls; ++poll) {
    /* Build a write value in which only the bits NOT in `mask`
     * remain set; those are the bits we want to leave untouched.
     * Bits inside `mask` are written 0 to clear. */
    const uint16_t write_val = (uint16_t)(reg->WDTSR & (uint16_t)~mask);
    reg->WDTSR               = write_val;

#if defined(RA8_OFF_TARGET) && defined(UNIT_TEST)
    /* HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1260 */
    const bool cleared = ra8_fake_mmio_poll(&reg->WDTSR, poll, ((uint16_t)reg->WDTSR & mask) == 0U);
#else
    /* HUM Ch 27.2.3 "WDTSR : WDT Status Register" p 1260 */
    const bool cleared = ((uint16_t)reg->WDTSR & mask) == 0U;
#endif
    if (cleared) {
      return k_ra8_ok;
    }
  }

  ra8_log_error(s_tag, "wdt_clear_status_blocking timed out");
  return k_ra8_err_hw_timeout;
}

[[nodiscard]] ra8_err_t ra8_wdt_get_counter(uint16_t* out_count)
{
  RA8_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 -- the live
   * down-counter occupies CNTVAL[13:0]. */
  *out_count = (uint16_t)(ra8_wdt()->WDTSR & k_ra8_wdt_sr_cnt_mask);
  return k_ra8_ok;
}

/* ra8_wdt_timeout_cycles_get, ra8_wdt_pclkb_divisor and
 * ra8_wdt_total_pclkb_cycles are defined in src/wdt_timing_abi.zig
 * (RA8FW-889). */

/* The subscriber table, ra8_wdt_dispatch, ra8_wdt_deinit, the NMI
 * wiring and ra8_wdt_enter_stop / ra8_wdt_exit_stop are defined in
 * src/wdt_subs_abi.zig (RA8FW-890). */

/* ra8_wdt_ofs_get and ra8_wdt_ofs_reader_set (the OFS0 / OFS3 read-only
 * decode) are defined in src/wdt_ofs_abi.zig (RA8FW-888). */
