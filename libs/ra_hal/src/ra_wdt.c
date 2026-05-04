/**
 * @file ra_wdt.c
 * @brief Software Watchdog Timer (WDT) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Register-start-mode driver for the RA8D2 WDT (HUM Ch 27,
 * p 1256-1270). The companion IWDT lives in ``ra_iwdt.c``; do not
 * confuse the two.
 *
 * ## OFS0 relationship
 *
 * In auto-start mode the OFS0 option-setting register (HUM Ch 7)
 * latches every period / clock-divider / window / reset-vs-NMI /
 * Sleep-stop bit *before* this driver runs, and the runtime WDTCR /
 * WDTRCR / WDTCSTPR registers become read-only-as-zero. This driver
 * therefore makes ``ra_wdt_init()``'s register writes harmless
 * no-ops in that mode while still issuing the first refresh -- so
 * the same call site works in either configuration. The decoded
 * OFSm view is exposed by ``ra_wdt_ofs_get()`` so the application
 * can introspect what the boot ROM latched.
 *
 * In register-start mode (``OFS0.WDT0STRT = 1``) the same
 * ``ra_wdt_init()`` writes WDTCR / WDTRCR / WDTCSTPR exactly once and
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
 * ``ra_wdt_install_nmi`` enables that bit (and clears any stale
 * status); the dispatch entry point ``ra_wdt_dispatch`` is what the
 * NMI handler calls.
 *
 * ## Multi-subscriber model
 *
 * The driver maintains a static ``k_ra_wdt_max_subs`` slot table so
 * several modules (state-of-health logger, crash recorder, app
 * cleanup task, ...) can fan-out the same event without any module
 * owning the single callback slot exclusively. The legacy single-
 * callback ``ra_wdt_attach_handler`` still works -- it owns one
 * dedicated slot in the same table.
 *
 * Every register access carries a HUM Ch 27.x citation.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_wdt.h"

#include <stdint.h>

#include "ra8d2_wdt_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_icu.h"
#include "ra_log.h"

/**
 * @var s_tag
 * @brief Module log tag.
 *
 * @details
 * Passed as the first argument to ``ra_log_*`` so log lines are
 * easy to grep.
 *
 * @note Read-only; do not modify.
 */
static const char* s_tag = "WDT";

/**
 * @struct ra_wdt_sub_t
 * @brief One entry in the multi-subscriber dispatch table.
 *
 * @details
 * ``fn == nullptr`` marks a free slot. The driver linearly scans the
 * table -- with ``k_ra_wdt_max_subs == 6`` that is well within the
 * NMI-handler latency budget.
 */
typedef struct {
  ra_wdt_event_fn_t fn;  /**< Subscriber callback or ``nullptr`` if slot is free. */
  void*             ctx; /**< Opaque pointer forwarded to ``fn``.                 */
} ra_wdt_sub_t;

/**
 * @enum ra_wdt_legacy_slot_t
 * @brief Slot reserved for the legacy single-callback API.
 *
 * @details
 * ``ra_wdt_attach_handler`` shares the same dispatch table as the
 * multi-subscriber API but always lives in slot 0; that way both
 * APIs can coexist without one tearing down the other's entry.
 */
typedef enum : uint8_t {
  k_ra_wdt_legacy_slot = 0U,
} ra_wdt_legacy_slot_t;

/**
 * @var s_wdt_subs
 * @brief Dispatch table of registered subscribers.
 *
 * @details
 * Static storage; cleared at C startup and again during
 * ``ra_wdt_deinit``. Slot 0 is owned by the legacy single-callback
 * API; slots 1..k_ra_wdt_max_subs-1 are claimed by ``ra_wdt_subscribe``.
 *
 * @warning Do not write directly -- always go through the public
 *          ``ra_wdt_*`` API.
 */
static ra_wdt_sub_t s_wdt_subs[k_ra_wdt_max_subs];

/**
 * @brief Default OFSm reader: dereferences the address as a 32-bit word.
 *
 * @details
 * Used when ``ra_wdt_ofs_reader_set(nullptr)`` is the active reader
 * choice. On the target this maps to a real flash / MRAM read; in
 * unit tests the ``ra_wdt_ofs_reader_set`` hook is normally swapped
 * for a stub that returns canned data.
 *
 * @param[in] ofs_addr See implementation.
 * @param[in] out_word See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_default_ofs_reader(uintptr_t ofs_addr, uint32_t* out_word)
{
  if (out_word == nullptr) {
    return k_ra_err_null_ptr;
  }
  /* HUM Ch 7 "Option-Setting Memory" -- OFSm lives in MRAM and is
   * mapped read-only into the application address space. */
  const volatile uint32_t* src = (const volatile uint32_t*)ofs_addr;
  *out_word                    = *src;
  return k_ra_ok;
}

/**
 * @var s_ofs_reader
 * @brief Active OFSm reader hook (dependency-inverted for test isolation).
 */
static ra_wdt_ofs_reader_fn_t s_ofs_reader = internal_default_ofs_reader;

/**
 * @enum ra_wdt_status_combined_t
 * @brief Union of the two WDTSR top-flag bits the driver cares about.
 */
typedef enum : uint16_t {
  k_ra_wdt_status_all = k_ra_wdt_status_underflow | k_ra_wdt_status_refresh,
} ra_wdt_status_combined_t;

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
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_clock_div_is_valid(ra_wdt_clock_div_t div)
{
  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 -- legal
   * CKS[3:0] encodings are 0x1, 0x4, 0x6, 0x7, 0x8, 0xF only. */
  switch (div) {
    case k_ra_wdt_clkdiv_4:
    case k_ra_wdt_clkdiv_64:
    case k_ra_wdt_clkdiv_128:
    case k_ra_wdt_clkdiv_512:
    case k_ra_wdt_clkdiv_2048:
    case k_ra_wdt_clkdiv_8192:
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
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_timeout_sel_is_valid(ra_wdt_timeout_sel_t sel)
{
  /* HUM Ch 27.2.2 "WDTCR" p 1259 */
  bool ok = false;
  switch (sel) {
    case k_ra_wdt_timeout_1024:
    case k_ra_wdt_timeout_4096:
    case k_ra_wdt_timeout_8192:
    case k_ra_wdt_timeout_16384:
      ok = true;
      break;
    default:
      ok = false;
      break;
  }
  return ok;
}

/**
 * @brief Pack a ra_wdt_cfg_t into a 16-bit WDTCR word.
 *
 * @param[in] cfg Caller-validated configuration block.
 * @return The 16-bit value to write into WDTCR.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint16_t internal_pack_wdtcr(const ra_wdt_cfg_t* cfg)
{
  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 */
  const uint16_t tops = (uint16_t)((uint16_t)cfg->timeout & k_ra_wdt_mask_tops);
  const uint16_t cks  = (uint16_t)((uint16_t)cfg->clock_div & k_ra_wdt_mask_cks);
  const uint16_t rpes = (uint16_t)((uint16_t)cfg->window_end & k_ra_wdt_mask_rpes);
  const uint16_t rpss = (uint16_t)((uint16_t)cfg->window_start & k_ra_wdt_mask_rpss);

  uint16_t word = 0U;
  word |= (uint16_t)(tops << k_ra_wdt_shift_tops);
  word |= (uint16_t)(cks << k_ra_wdt_shift_cks);
  word |= (uint16_t)(rpes << k_ra_wdt_shift_rpes);
  word |= (uint16_t)(rpss << k_ra_wdt_shift_rpss);
  return word;
}

/**
 * @brief Clear every subscriber slot.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_subs_clear_all(void)
{
  for (uint8_t i = 0U; i < k_ra_wdt_max_subs; ++i) {
    s_wdt_subs[i].fn  = nullptr;
    s_wdt_subs[i].ctx = nullptr;
  }
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_init(const ra_wdt_cfg_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  if (!internal_clock_div_is_valid(cfg->clock_div)) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_timeout_sel_is_valid(cfg->timeout)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_wdt_regs_t* reg = ra_wdt();

  /* HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258 -- one
   * 16-bit transaction commits the timeout / divider / window. */
  reg->WDTCR = internal_pack_wdtcr(cfg);

  /* HUM Ch 27.2.4 "WDTRCR : WDT Reset Control Register", p 1262 --
   * RSTIRQS selects internal reset (1) vs NMI / IRQ (0) on
   * underflow or refresh-error. */
  reg->WDTRCR = (uint8_t)((cfg->on_expiry == k_ra_wdt_on_expiry_reset) ? k_ra_wdt_rcr_rstirqs : 0U);

  /* HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register",
   * p 1262 -- SLCSTP halts the counter on Sleep / Deep Sleep. */
  reg->WDTCSTPR =
    (uint8_t)((cfg->stop_in_sleep == k_ra_wdt_sleep_stop_count) ? k_ra_wdt_cstpr_slcstp : 0U);

  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 -- the
   * 0x00 / 0xFF unlock sequence is what arms the down-counter in
   * register-start mode, and is harmless in auto-start mode. */
  ra_wdt_refresh();

  ra_log_info(s_tag, "wdt_init armed");
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_deinit(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register",
   * p 1262 -- the strongest "off" the driver can request is to halt
   * the counter while the CPU is asleep. The peripheral itself
   * cannot be disarmed once started. */
  reg->WDTCSTPR = k_ra_wdt_cstpr_slcstp;
  internal_subs_clear_all();
  ra_log_info(s_tag, "wdt_deinit (sleep-stop set)");
  return k_ra_ok;
}

/* =============================================================================
 * Refresh
 * =============================================================================
 */

/* Implementation of ra_wdt_refresh_deferred (see header for full contract) -- see header for the documented contract. */
void ra_wdt_refresh_deferred(void)
{
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  ra_wdt_refresh();
}

[[nodiscard]] ra_err_t ra_wdt_refresh_for(ra_wdt_instance_t which)
{
  if ((uint8_t)which >= k_ra_wdt_instance_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 27.2.1 "WDTRR : WDT Refresh Register", p 1257 */
  ra_wdt_refresh_instance(which);
  return k_ra_ok;
}

/* =============================================================================
 * Status read / clear / counter
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_get_status(uint16_t* out_mask)
{
  RA_CHECK_NULL_PTR(out_mask, s_tag, "out_mask must not be nullptr");
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 */
  *out_mask = (uint16_t)(ra_wdt()->WDTSR & k_ra_wdt_status_all);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_clear_status(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 -- UNDFF /
   * REFEF are write-0-to-clear; writing 1 has no effect. We mask
   * out the flag bits and write the result back. */
  reg->WDTSR = (uint16_t)(reg->WDTSR & (uint16_t)~k_ra_wdt_status_all);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_clear_status_blocking(uint16_t mask)
{
  if ((mask & ~k_ra_wdt_status_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (mask == k_ra_wdt_status_none) {
    return k_ra_ok;
  }

  volatile r_wdt_regs_t* reg = ra_wdt();

  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1261 -- W0C
   * sequence: write the inverse of the bits we want to clear back
   * into WDTSR, then poll until the targeted bits read 0. The
   * (N + 1) PCLKB cycle latency is bounded by the divider; we
   * cap our polls at k_ra_wdt_clear_max_polls (16384). */
  for (uint32_t poll = 0U; poll < k_ra_wdt_clear_max_polls; ++poll) {
    /* Build a write value in which only the bits NOT in `mask`
     * remain set; those are the bits we want to leave untouched.
     * Bits inside `mask` are written 0 to clear. */
    const uint16_t write_val = (uint16_t)(reg->WDTSR & (uint16_t)~mask);
    reg->WDTSR               = write_val;

    if (((uint16_t)reg->WDTSR & mask) == 0U) {
      return k_ra_ok;
    }
  }

  ra_log_error(s_tag, "wdt_clear_status_blocking timed out");
  return k_ra_err_hw_timeout;
}

[[nodiscard]] ra_err_t ra_wdt_get_counter(uint16_t* out_count)
{
  RA_CHECK_NULL_PTR(out_count, s_tag, "out_count must not be nullptr");
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 -- the live
   * down-counter occupies CNTVAL[13:0]. */
  *out_count = (uint16_t)(ra_wdt()->WDTSR & k_ra_wdt_sr_cnt_mask);
  return k_ra_ok;
}

/* =============================================================================
 * Timeout decoding (FSP timeoutGet analogue)
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_timeout_cycles_get(ra_wdt_timeout_sel_t sel, uint16_t* out_cycles)
{
  RA_CHECK_NULL_PTR(out_cycles, s_tag, "out_cycles must not be nullptr");
  /* HUM Ch 27.2.2 "WDTCR" p 1259 */
  switch (sel) {
    case k_ra_wdt_timeout_1024:
      *out_cycles = k_ra_wdt_cycles_1024;
      return k_ra_ok;
    case k_ra_wdt_timeout_4096:
      *out_cycles = k_ra_wdt_cycles_4096;
      return k_ra_ok;
    case k_ra_wdt_timeout_8192:
      *out_cycles = k_ra_wdt_cycles_8192;
      return k_ra_ok;
    case k_ra_wdt_timeout_16384:
      *out_cycles = k_ra_wdt_cycles_16384;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
  }
}

[[nodiscard]] ra_err_t ra_wdt_pclkb_divisor(ra_wdt_clock_div_t div, uint16_t* out_divisor)
{
  RA_CHECK_NULL_PTR(out_divisor, s_tag, "out_divisor must not be nullptr");
  /* HUM Ch 27.2.2 "WDTCR" p 1258 */
  switch (div) {
    case k_ra_wdt_clkdiv_4:
      *out_divisor = k_ra_wdt_div_value_4;
      return k_ra_ok;
    case k_ra_wdt_clkdiv_64:
      *out_divisor = k_ra_wdt_div_value_64;
      return k_ra_ok;
    case k_ra_wdt_clkdiv_128:
      *out_divisor = k_ra_wdt_div_value_128;
      return k_ra_ok;
    case k_ra_wdt_clkdiv_512:
      *out_divisor = k_ra_wdt_div_value_512;
      return k_ra_ok;
    case k_ra_wdt_clkdiv_2048:
      *out_divisor = k_ra_wdt_div_value_2048;
      return k_ra_ok;
    case k_ra_wdt_clkdiv_8192:
      *out_divisor = k_ra_wdt_div_value_8192;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
  }
}

[[nodiscard]] ra_err_t ra_wdt_total_pclkb_cycles(ra_wdt_timeout_sel_t sel,
                                                 ra_wdt_clock_div_t   div,
                                                 uint32_t*            out_pclkb_cycles)
{
  RA_CHECK_NULL_PTR(out_pclkb_cycles, s_tag, "out_pclkb_cycles must not be nullptr");

  uint16_t       cycles  = 0U;
  uint16_t       divisor = 0U;
  const ra_err_t e1      = ra_wdt_timeout_cycles_get(sel, &cycles);
  if (e1 != k_ra_ok) {
    return e1;
  }
  const ra_err_t e2 = ra_wdt_pclkb_divisor(div, &divisor);
  if (e2 != k_ra_ok) {
    return e2;
  }

  /* Largest product is 16384 * 8192 = 134_217_728 -- fits 32 bits. */
  *out_pclkb_cycles = (uint32_t)cycles * (uint32_t)divisor;
  return k_ra_ok;
}

/* =============================================================================
 * Single + multi-subscriber callback API
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_attach_handler(ra_wdt_event_fn_t fn, void* ctx)
{
  /* The legacy attach API maps to the reserved slot 0 -- that way
   * both APIs share the same dispatch table without stomping each
   * other. ``fn == nullptr`` clears the slot. */
  s_wdt_subs[k_ra_wdt_legacy_slot].fn  = fn;
  s_wdt_subs[k_ra_wdt_legacy_slot].ctx = ctx;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_subscribe(ra_wdt_event_fn_t fn, void* ctx, uint8_t* out_slot)
{
  RA_CHECK_NULL_PTR(fn, s_tag, "fn must not be nullptr");

  /* Walk the table starting at the first non-legacy slot; slot 0 is
   * reserved for ``ra_wdt_attach_handler``. */
  for (uint8_t i = 1U; i < k_ra_wdt_max_subs; ++i) {
    if (s_wdt_subs[i].fn == nullptr) {
      s_wdt_subs[i].fn  = fn;
      s_wdt_subs[i].ctx = ctx;
      if (out_slot != nullptr) {
        *out_slot = i;
      }
      return k_ra_ok;
    }
  }

  ra_log_error(s_tag, "wdt_subscribe table full");
  return k_ra_err_no_mem;
}

[[nodiscard]] ra_err_t ra_wdt_unsubscribe(uint8_t slot)
{
  if (slot >= k_ra_wdt_max_subs) {
    return k_ra_err_invalid_arg;
  }
  if (s_wdt_subs[slot].fn == nullptr) {
    return k_ra_err_not_found;
  }
  s_wdt_subs[slot].fn  = nullptr;
  s_wdt_subs[slot].ctx = nullptr;
  return k_ra_ok;
}

/* Implementation of ra_wdt_subscriber_count (see header for full contract) -- see header for the documented contract. */
uint8_t ra_wdt_subscriber_count(void)
{
  uint8_t count = 0U;
  for (uint8_t i = 0U; i < k_ra_wdt_max_subs; ++i) {
    if (s_wdt_subs[i].fn != nullptr) {
      ++count;
    }
  }
  return count;
}

/* Implementation of ra_wdt_dispatch (see header for full contract) -- see header for the documented contract. */
void ra_wdt_dispatch(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.3 "WDTSR : WDT Status Register", p 1260 -- snapshot
   * the flags before clearing, then hand the latched mask to every
   * registered subscriber. */
  const uint16_t mask = (uint16_t)(reg->WDTSR & k_ra_wdt_status_all);
  reg->WDTSR          = (uint16_t)(reg->WDTSR & (uint16_t)~mask);

  for (uint8_t i = 0U; i < k_ra_wdt_max_subs; ++i) {
    const ra_wdt_event_fn_t fn  = s_wdt_subs[i].fn;
    void* const             ctx = s_wdt_subs[i].ctx;
    if (fn != nullptr) {
      fn(ctx, mask);
    }
  }
}

/* =============================================================================
 * NMI vector wiring (HUM Ch 14.2.14 p 542)
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_install_nmi(void)
{
  /* HUM Ch 14.2.15 "NMICLR" p 544 -- W1C any stale WDT NMI status
   * before unmasking the source so we don't immediately fire on a
   * pre-existing latched flag. */
  const ra_err_t e_clr = ra_icu_nmi_clear(k_ra_wdt_nmier_wdten_mask);
  if (e_clr != k_ra_ok) {
    return e_clr;
  }
  /* HUM Ch 14.2.14 "NMIER" p 542 -- WDTEN bit 1 (0x2) routes the
   * WDT0 underflow / refresh-error to the Cortex-M85 NMI line. */
  return ra_icu_nmi_enable(k_ra_wdt_nmier_wdten_mask);
}

[[nodiscard]] ra_err_t ra_wdt_uninstall_nmi(void)
{
  /* HUM Ch 14.2.14 "NMIER" p 542 */
  const ra_err_t e_dis = ra_icu_nmi_disable(k_ra_wdt_nmier_wdten_mask);
  if (e_dis != k_ra_ok) {
    return e_dis;
  }
  /* HUM Ch 14.2.15 "NMICLR" p 544 */
  return ra_icu_nmi_clear(k_ra_wdt_nmier_wdten_mask);
}

/* =============================================================================
 * Sleep-mode stop control
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_enter_stop(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register",
   * p 1262 -- set SLCSTP to halt the counter on Sleep entry. */
  reg->WDTCSTPR = k_ra_wdt_cstpr_slcstp;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_exit_stop(void)
{
  volatile r_wdt_regs_t* reg = ra_wdt();
  /* HUM Ch 27.2.5 "WDTCSTPR : WDT Count Stop Control Register",
   * p 1262 -- clear SLCSTP so the counter keeps running in Sleep. */
  reg->WDTCSTPR = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * OFS0 / OFS3 read-only decode
 * =============================================================================
 */

[[nodiscard]] ra_err_t ra_wdt_ofs_reader_set(ra_wdt_ofs_reader_fn_t reader)
{
  s_ofs_reader = (reader == nullptr) ? internal_default_ofs_reader : reader;
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_wdt_ofs_get(ra_wdt_instance_t which, ra_wdt_ofs_decoded_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if ((uint8_t)which >= k_ra_wdt_instance_count) {
    return k_ra_err_invalid_arg;
  }

  /* HUM Ch 27.2.6 "Option Function Select Register" p 1262 + Table
   * 27.5 p 1269 -- read the appropriate OFSm word from option-
   * setting memory and decode the seven WDT fields back out.
   *
   * The WDT driver does NOT write OFSm; only ``ra_ofs.c`` may emit
   * the option-setting sections. We go through the reader hook so
   * unit tests can inject canned OFSm contents. */
  const uintptr_t addr = ra_wdt_ofs_addr(which);
  uint32_t        ofsm = 0U;
  const ra_err_t  e    = s_ofs_reader(addr, &ofsm);
  if (e != k_ra_ok) {
    return e;
  }

  const uint8_t strt = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_strt) & k_ra_wdt_ofs_mask_strt);
  const uint8_t tops = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_tops) & k_ra_wdt_ofs_mask_tops);
  const uint8_t cks  = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_cks) & k_ra_wdt_ofs_mask_cks);
  const uint8_t rpes = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_rpes) & k_ra_wdt_ofs_mask_rpes);
  const uint8_t rpss = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_rpss) & k_ra_wdt_ofs_mask_rpss);
  const uint8_t rstirqs =
    (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_rstirqs) & k_ra_wdt_ofs_mask_rstirqs);
  const uint8_t stpctl = (uint8_t)((ofsm >> k_ra_wdt_ofs_shift_stpctl) & k_ra_wdt_ofs_mask_stpctl);

  out->cfg.timeout       = (ra_wdt_timeout_sel_t)tops;
  out->cfg.clock_div     = (ra_wdt_clock_div_t)cks;
  out->cfg.window_start  = (ra_wdt_window_start_t)rpss;
  out->cfg.window_end    = (ra_wdt_window_end_t)rpes;
  out->cfg.on_expiry     = (ra_wdt_reset_ctrl_t)rstirqs;
  out->cfg.stop_in_sleep = (ra_wdt_stop_ctrl_t)stpctl;
  out->start_mode        = (ra_wdt_ofs_strt_t)strt;
  out->auto_start        = (out->start_mode == k_ra_wdt_ofs_strt_auto);
  return k_ra_ok;
}
