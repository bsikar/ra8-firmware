/**
 * @file ra8_wdt.h
 * @brief Software Watchdog Timer (WDT) driver header
 * @ingroup grp_hal_timers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * This is the *software*-controlled Watchdog Timer described in HUM
 * Ch 27 (p 1256-1270). It is distinct from the IWDT driver
 * (``ra8_iwdt.h``, HUM Ch 28) which has its own free-running clock
 * and cannot be reconfigured at runtime.
 *
 * ## Start mode and OFS0
 *
 * The WDT has two start modes selected by the ``OFS0.WDT0STRT`` bit
 * in option-setting memory (HUM Ch 7, "Option-Setting Memory") for
 * WDT0, and by ``OFS3.WDT1STRT`` for the M33-side WDT1:
 *
 * - **Auto-start mode** (``WDT0STRT = 0``): the period, clock
 * divider, window, reset-vs-NMI behaviour, and Sleep stop control
 * are *all* baked in to the OFS0 word at flash time. The runtime
 * WDTCR / WDTRCR / WDTCSTPR registers become read-only-as-zero --
 * this driver's ``ra8_wdt_init`` is a no-op in that build, but
 * refresh, status and dispatch still work.
 * - **Register-start mode** (``WDT0STRT = 1``): the counter does
 * *not* start out of reset. Software must programme
 * WDTCR / WDTRCR / WDTCSTPR and then issue the first WDTRR refresh
 * to arm the down-counter. This is the mode ``ra8_wdt_init``
 * expects.
 *
 * The driver does not (and cannot) flip OFS0 itself -- ``ra8_ofs.c``
 * owns the writable side. ``ra8_wdt_ofs_get`` exposes the read-only
 * view so the application can decode whichever mode the boot ROM
 * latched.
 *
 * ## API surface
 *
 * - ``ra8_wdt_init(cfg)`` -- programme WDTCR/WDTRCR/WDTCSTPR and arm
 * the counter via the first refresh (register-start mode only).
 * - ``ra8_wdt_deinit`` -- snapshot WDTCSTPR so the counter halts on
 * Sleep entry. Auto-started WDTs *cannot* truly be stopped.
 * - ``ra8_wdt_refresh_deferred`` -- run-time heartbeat (WDT0).
 * - ``ra8_wdt_refresh_for(which)`` -- per-instance heartbeat.
 * - ``ra8_wdt_get_status`` / ``ra8_wdt_clear_status`` /
 * ``ra8_wdt_clear_status_blocking`` -- UNDFF / REFEF flag query
 * and clear (with W0C semantics + bounded busy-wait).
 * - ``ra8_wdt_get_counter`` -- read live CNTVAL[13:0].
 * - ``ra8_wdt_timeout_cycles_get`` / ``ra8_wdt_pclkb_divisor`` --
 * decode TOPS + CKS into clock-cycle counts (FSP timeoutGet).
 * - ``ra8_wdt_attach_handler`` (legacy single-callback API) and
 * ``ra8_wdt_subscribe`` / ``ra8_wdt_unsubscribe`` (multi-
 * subscriber API) plus ``ra8_wdt_dispatch`` -- NMI / underflow
 * callback injection.
 * - ``ra8_wdt_install_nmi`` / ``ra8_wdt_uninstall_nmi`` -- enable
 * or disable the WDT bit in NMIER (full NMI vector wiring).
 * - ``ra8_wdt_enter_stop`` / ``ra8_wdt_exit_stop`` -- Sleep-mode
 * stop-control toggle.
 * - ``ra8_wdt_ofs_get(which, *out)`` -- decode the OFSm option-
 * setting word for the targeted instance into ``ra8_wdt_cfg_t``
 * plus the auto-start flag.
 *
 * ## State machine
 *
 * @par State Machine:
 * @dot
 * digraph ra8_wdt_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Reset [label="Reset"];
 *   Configured [label="Configured"];
 *   Running [label="Running"];
 *   Underflow [label="Underflow"];
 *   RefreshErr [label="RefreshErr"];
 *
 *   __start -> Reset;
 *   Reset -> Configured [label="ra8_wdt_init\n(register-start)"];
 *   Reset -> Running [label="OFS0.WDT0STRT == 0\n(auto-start)"];
 *   Configured -> Running [label="ra8_wdt_refresh_deferred\n(first refresh)"];
 *   Running -> Running [label="ra8_wdt_refresh_deferred\ninside window"];
 *   Running -> Underflow [label="counter reaches zero"];
 *   Running -> RefreshErr [label="refresh outside window"];
 *   Underflow -> Reset [label="WDTRCR.RSTIRQS == 1"];
 *   Underflow -> Running [label="WDTRCR.RSTIRQS == 0 && NMI\nhandled"];
 *   RefreshErr -> Reset [label="WDTRCR.RSTIRQS == 1"];
 *   RefreshErr -> Running [label="WDTRCR.RSTIRQS == 0 && NMI\nhandled"];
 * }
 * @enddot
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_wdt_regs.h"

/**
 * @enum ra8_wdt_clock_div_t
 * @brief Legal CKS[3:0] encodings for WDTCR.
 *
 * @details
 * Only six of the sixteen 4-bit CKS encodings are usable -- HUM
 * Ch 27.2.2 p 1258 marks the rest as "Setting prohibited". Each
 * value is the raw bit pattern that must land in WDTCR.CKS, so the
 * driver can ``& k_ra8_wdt_mask_cks`` and ``<< k_ra8_wdt_shift_cks``
 * straight from this enum.
 *
 * @see HUM Ch 27.2.2 "WDTCR : WDT Control Register", p 1258.
 */
typedef enum : uint8_t {
  k_ra8_wdt_clkdiv_4    = 0x1U, /**< PCLKB / 4.    */
  k_ra8_wdt_clkdiv_64   = 0x4U, /**< PCLKB / 64.   */
  k_ra8_wdt_clkdiv_128  = 0xFU, /**< PCLKB / 128.  */
  k_ra8_wdt_clkdiv_512  = 0x6U, /**< PCLKB / 512.  */
  k_ra8_wdt_clkdiv_2048 = 0x7U, /**< PCLKB / 2048. */
  k_ra8_wdt_clkdiv_8192 = 0x8U, /**< PCLKB / 8192. */
} ra8_wdt_clock_div_t;

/**
 * @enum ra8_wdt_timeout_sel_t
 * @brief Legal TOPS[1:0] encodings for WDTCR.
 *
 * @details
 * Selects the WDT timeout in counter cycles before the divider is
 * applied. Decode with ``ra8_wdt_timeout_cycles_get`` to recover the
 * numeric cycle count, or with ``ra8_wdt_total_pclkb_cycles`` to fold
 * in the CKS divider.
 *
 * @see HUM Ch 27.2.2 Table 27.2 p 1259.
 */
typedef enum : uint8_t {
  k_ra8_wdt_timeout_1024  = 0x0U, /**< 1024 cycles.  */
  k_ra8_wdt_timeout_4096  = 0x1U, /**< 4096 cycles.  */
  k_ra8_wdt_timeout_8192  = 0x2U, /**< 8192 cycles.  */
  k_ra8_wdt_timeout_16384 = 0x3U, /**< 16384 cycles. */
} ra8_wdt_timeout_sel_t;

/**
 * @enum ra8_wdt_window_start_t
 * @brief Legal RPSS[1:0] encodings (refresh-permitted window start).
 *
 * @details
 * Selects the *upper* bound of the refresh-permitted window, expressed
 * as a percentage of the full timeout. ``window_start_100`` disables
 * the upper bound. HUM Ch 27.2.2 p 1259.
 */
typedef enum : uint8_t {
  k_ra8_wdt_window_start_25  = 0x0U, /**< 25 %.                   */
  k_ra8_wdt_window_start_50  = 0x1U, /**< 50 %.                   */
  k_ra8_wdt_window_start_75  = 0x2U, /**< 75 %.                   */
  k_ra8_wdt_window_start_100 = 0x3U, /**< 100 % (no upper bound). */
} ra8_wdt_window_start_t;

/**
 * @enum ra8_wdt_window_end_t
 * @brief Legal RPES[1:0] encodings (refresh-permitted window end).
 *
 * @details
 * Selects the *lower* bound of the refresh-permitted window, expressed
 * as a percentage of the full timeout. ``window_end_0`` disables the
 * lower bound. HUM Ch 27.2.2 p 1259.
 *
 * @invariant ``window_end`` < ``window_start`` -- silicon forces
 *            ``window_end = 0%`` if the constraint is violated.
 */
typedef enum : uint8_t {
  k_ra8_wdt_window_end_75 = 0x0U, /**< 75 %.                 */
  k_ra8_wdt_window_end_50 = 0x1U, /**< 50 %.                 */
  k_ra8_wdt_window_end_25 = 0x2U, /**< 25 %.                 */
  k_ra8_wdt_window_end_0  = 0x3U, /**< 0 % (no lower bound). */
} ra8_wdt_window_end_t;

/**
 * @enum ra8_wdt_reset_ctrl_t
 * @brief WDTRCR.RSTIRQS encoding -- reset vs NMI on expiry.
 *
 * @details
 * Determines what the WDT does when it underflows or sees a
 * refresh-error. ``on_expiry_nmi`` routes the event through
 * ICU.NMIER (HUM Ch 14.2.14 p 542) where ``ra8_wdt_install_nmi``
 * picks it up; ``on_expiry_reset`` triggers an internal reset.
 *
 * @see HUM Ch 27.2.4 "WDTRCR" p 1262.
 */
typedef enum : uint8_t {
  k_ra8_wdt_on_expiry_nmi   = 0U, /**< RSTIRQS=0 -- route to NMI / IRQ. */
  k_ra8_wdt_on_expiry_reset = 1U, /**< RSTIRQS=1 -- internal reset.     */
} ra8_wdt_reset_ctrl_t;

/**
 * @enum ra8_wdt_stop_ctrl_t
 * @brief WDTCSTPR.SLCSTP encoding -- counter behaviour in Sleep.
 *
 * @details
 * ``sleep_keep_count`` keeps the counter ticking through Sleep / Deep
 * Sleep so the WDT can still fire if the wake event is missed.
 * ``sleep_stop_count`` halts the counter on Sleep entry, useful for
 * battery / low-power operation.
 *
 * @see HUM Ch 27.2.5 "WDTCSTPR" p 1262.
 */
typedef enum : uint8_t {
  k_ra8_wdt_sleep_keep_count = 0U, /**< SLCSTP=0 -- counter runs in Sleep.  */
  k_ra8_wdt_sleep_stop_count = 1U, /**< SLCSTP=1 -- counter halts in Sleep. */
} ra8_wdt_stop_ctrl_t;

/**
 * @enum ra8_wdt_ofs_strt_t
 * @brief OFSm.WDTnSTRT encoding -- auto vs register start mode.
 *
 * @details
 * The boot ROM latches this bit out of the OFS0 (WDT0) or OFS3 (WDT1)
 * option-setting word at reset. ``auto`` means WDTCR / WDTRCR /
 * WDTCSTPR are all sourced from OFSm; ``register_start`` means they
 * are programmable via the runtime registers exactly once after
 * reset.
 *
 * @see HUM Ch 27.3.1 p 1263.
 */
typedef enum : uint8_t {
  k_ra8_wdt_ofs_strt_auto     = 0U, /**< Auto-start (OFSm-driven).        */
  k_ra8_wdt_ofs_strt_register = 1U, /**< Register-start (runtime config). */
} ra8_wdt_ofs_strt_t;

/**
 * @enum ra8_wdt_status_mask_t
 * @brief WDTSR top-bit status flags.
 *
 * @details
 * Mirrors the high two bits of ``WDTSR`` -- see HUM Ch 27.2.3 p 1260.
 */
typedef enum : uint16_t {
  k_ra8_wdt_status_none      = 0x0000U,            /**< No flag set.    */
  k_ra8_wdt_status_underflow = k_ra8_wdt_sr_undff, /**< UNDFF (bit 14). */
  k_ra8_wdt_status_refresh   = k_ra8_wdt_sr_refef, /**< REFEF (bit 15). */
} ra8_wdt_status_mask_t;

/**
 * @enum ra8_wdt_subs_count_t
 * @brief Maximum number of hot-pluggable subscribers per WDT instance.
 *
 * @details
 * Sized to cover the typical fan-out of underflow / refresh-error
 * handlers (state-of-health logger, watchdog-aware task cleanup,
 * crash-logger, and one or two app callbacks). All slots are
 * statically allocated -- no malloc per the project's rules.
 */
typedef enum : uint8_t {
  k_ra8_wdt_max_subs = 6U, /**< Max simultaneous subscribers. */
} ra8_wdt_subs_count_t;

/**
 * @enum ra8_wdt_clear_timeout_t
 * @brief Bounded retry budget for ``ra8_wdt_clear_status_blocking``.
 *
 * @details
 * HUM Ch 27.2.3 p 1261 says clearing UNDFF / REFEF takes ``N + 1``
 * PCLKB cycles, where ``N`` depends on the divider (worst case
 * ``8192 + 1``). The blocking helper polls the flag, decrementing a
 * read-back counter sized for that worst case (with a generous
 * margin) before returning ``k_ra8_err_hw_timeout``.
 */
typedef enum : uint32_t {
  k_ra8_wdt_clear_max_polls = 0x4000UL, /**< 16384 reads -- safely > 8193. */
} ra8_wdt_clear_timeout_t;

/**
 * @struct ra8_wdt_cfg_t
 * @brief Register-start-mode configuration block.
 *
 * @details
 * Filled by the caller and passed to ``ra8_wdt_init``. Maps 1:1 onto
 * the bit-fields of WDTCR + WDTRCR + WDTCSTPR.
 *
 * @code
 * const ra8_wdt_cfg_t cfg = {
 *.timeout = k_ra8_wdt_timeout_4096,
 *.clock_div = k_ra8_wdt_clkdiv_128,
 *.window_start = k_ra8_wdt_window_start_100,
 *.window_end = k_ra8_wdt_window_end_0,
 *.on_expiry = k_ra8_wdt_on_expiry_reset,
 *.stop_in_sleep = k_ra8_wdt_sleep_stop_count,
 * };
 * (void)ra8_wdt_init(&cfg);
 * @endcode
 *
 * @invariant ``window_end`` < ``window_start`` (silicon forces end=0%
 * otherwise, per HUM Ch 27.2.2 p 1259).
 */
typedef struct {
  ra8_wdt_timeout_sel_t  timeout;       /**< WDTCR.TOPS[1:0] selection.     */
  ra8_wdt_clock_div_t    clock_div;     /**< WDTCR.CKS[3:0] selection.      */
  ra8_wdt_window_start_t window_start;  /**< WDTCR.RPSS[1:0] selection.     */
  ra8_wdt_window_end_t   window_end;    /**< WDTCR.RPES[1:0] selection.     */
  ra8_wdt_reset_ctrl_t   on_expiry;     /**< WDTRCR.RSTIRQS reset vs IRQ.   */
  ra8_wdt_stop_ctrl_t    stop_in_sleep; /**< WDTCSTPR.SLCSTP halt-on-Sleep. */
} ra8_wdt_cfg_t;

/**
 * @struct ra8_wdt_ofs_decoded_t
 * @brief Decoded view of an ``OFSm`` option-setting word.
 *
 * @details
 * Returned by ``ra8_wdt_ofs_get``. ``cfg`` mirrors the same five
 * fields as ``ra8_wdt_cfg_t`` (so the same caller code can compare
 * "what I asked for" vs "what the boot ROM latched"); ``auto_start``
 * tells which mode the chip booted in.
 *
 * @invariant If ``auto_start`` is true, the runtime WDTCR/WDTRCR/
 * WDTCSTPR writes have no effect (HUM Ch 27.3.1 p 1263).
 */
typedef struct {
  ra8_wdt_cfg_t      cfg;        /**< Decoded period / divider / window / etc. */
  ra8_wdt_ofs_strt_t start_mode; /**< OFSm.WDTnSTRT (auto vs register start).  */
  bool               auto_start; /**< Convenience flag = (start_mode == auto). */
} ra8_wdt_ofs_decoded_t;

/**
 * @typedef ra8_wdt_event_fn_t
 * @brief WDT NMI / underflow event callback.
 *
 * @param[in] ctx Caller context as registered with
 * ``ra8_wdt_attach_handler``.
 * @param[in] status_mask Latched WDTSR top bits at dispatch time
 * (``k_ra8_wdt_status_underflow`` and / or
 * ``k_ra8_wdt_status_refresh``).
 */
typedef void (*ra8_wdt_event_fn_t)(void* ctx, uint16_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the WDT in register-start mode.
 *
 * @details
 * Programmes WDTCR with the timeout / clock divider / window selection
 * from ``cfg``, WDTRCR with the reset-vs-NMI choice, and WDTCSTPR with
 * the Sleep-stop selection. After all three are written this function
 * issues the first refresh, which is what actually arms the counter.
 *
 * Algorithm:
 * 1. Validate ``cfg`` non-null.
 * 2. Validate ``cfg->clock_div`` against the legal CKS encodings.
 * 3. Build WDTCR by shifting each field into place.
 * 4. Write WDTCR (one 16-bit transaction), WDTRCR, WDTCSTPR.
 * 5. Issue the unlock sequence on WDTRR to arm the counter.
 *
 * In auto-start mode the WDTCR / WDTRCR / WDTCSTPR writes are silently
 * dropped by silicon, but the refresh still works -- so the function
 * is safe to call in either mode.
 *
 * @param[in] cfg Pointer to the configuration block.
 *
 * @return ``ra8_err_t`` Result code.
 * @retval k_ra8_ok Counter armed.
 * @retval k_ra8_err_null_ptr ``cfg`` was null.
 * @retval k_ra8_err_invalid_arg ``cfg->clock_div`` is not one of the
 * legal CKS encodings.
 *
 * @pre ``cfg`` is non-null.
 * @pre PCLKB has been started by the CGC driver.
 *
 * @post WDTCR / WDTRCR / WDTCSTPR reflect ``cfg`` (register-start mode).
 * @post WDTRR holds 0xFF -- the counter has been refreshed once.
 *
 * @note Not thread-safe -- call from init context only. The WDT
 * control registers can only be written once after reset (HUM
 * Ch 27.3.2 "Controlling Writes to the WDTCR, WDTRCR, and
 * WDTCSTPR Registers"), so subsequent calls have no effect.
 *
 * @see ra8_wdt_refresh_deferred Run-time heartbeat.
 * @see ra8_wdt_deinit Counterpart that disables in Sleep.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_init(const ra8_wdt_cfg_t* cfg);

/**
 * @brief Quiesce the driver into a Sleep-stop posture.
 *
 * @details
 * Sets WDTCSTPR.SLCSTP so the counter halts when the CPU enters
 * Sleep / Deep Sleep. The WDT itself cannot be disarmed once started,
 * so this is the closest analogue to a "deinit" the silicon allows.
 * Also clears every multi-subscriber slot so a follow-up ``ra8_wdt_init``
 * starts from a known callback table.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre ``ra8_wdt_init`` has been called.
 * @pre Caller is in a single-writer context.
 *
 * @post WDTCSTPR.SLCSTP = 1 (in register-start mode).
 * @post Counter still runs while CPU is awake.
 * @post Subscriber table is empty (every slot ``{nullptr, nullptr}``).
 *
 * @note No-op in auto-start mode -- WDTCSTPR is then read-only-zero.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_deinit(void);

/* =============================================================================
 * Refresh
 * =============================================================================
 */

/**
 * @brief Refresh the software WDT counter (WDT0).
 *
 * @details
 * Thin wrapper around the inline ``ra8_wdt_refresh`` helper in the
 * register header so other compilation units do not have to pull in
 * ``ra8_wdt_regs.h``.
 *
 * @pre WDT has been armed (auto-start out of reset, or
 * ``ra8_wdt_init`` succeeded).
 * @pre Refresh occurs strictly inside the refresh-permitted window
 * programmed by ``cfg->window_start`` / ``cfg->window_end``.
 *
 * @post WDTRR holds 0xFF.
 * @post Down-counter is reloaded.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
void ra8_wdt_refresh_deferred(void);

/**
 * @brief Refresh a specific WDT instance.
 *
 * @param[in] which Instance to refresh (k_ra8_wdt0 or k_ra8_wdt1).
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Counter refreshed.
 * @retval k_ra8_err_invalid_arg ``which`` not a known instance.
 *
 * @pre ``which`` < ``k_ra8_wdt_instance_count``.
 * @pre WDT has been armed.
 *
 * @post Targeted WDT's WDTRR holds 0xFF.
 * @post Targeted WDT down-counter is reloaded.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_refresh_for(ra8_wdt_instance_t which);

/* =============================================================================
 * Status read / clear / counter
 * =============================================================================
 */

/**
 * @brief Read the WDTSR status flag bits.
 *
 * @param[out] out_mask Receives ``k_ra8_wdt_status_underflow`` and / or
 * ``k_ra8_wdt_status_refresh`` (others = 0).
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Status read.
 * @retval k_ra8_err_null_ptr ``out_mask`` was null.
 *
 * @pre ``out_mask`` is non-null.
 * @pre WDT block has been powered (always-on for this peripheral).
 *
 * @post ``*out_mask`` is one of the ``k_ra8_wdt_status_*`` masks.
 * @post No side effect on WDTSR.
 *
 * @note Thread-safe wrt other readers; concurrent writers must
 * serialise the access.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_get_status(uint16_t* out_mask);

/**
 * @brief Clear the WDTSR underflow / refresh-error flags.
 *
 * @details
 * Writes 0 to UNDFF and REFEF (write-1 has no effect, per HUM
 * Ch 27.2.3 p 1261). The flag clear takes ``N + 1`` PCLKB cycles to
 * land, where ``N`` depends on the CKS divider; the driver does not
 * busy-wait, so an immediate read-back may still return the old
 * value. Use ``ra8_wdt_clear_status_blocking`` for the polled variant.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre WDT block has been powered.
 * @pre Caller is in a single-writer context.
 *
 * @post UNDFF / REFEF will read 0 within (N + 1) PCLKB cycles.
 * @post CNTVAL[13:0] is unaffected.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_clear_status(void);

/**
 * @brief Clear specific WDTSR flag bits and busy-wait until they read 0.
 *
 * @details
 * HUM Ch 27.2.3 p 1261 documents that the W0C flag clear takes
 * ``N + 1`` PCLKB cycles to land. This helper polls the register up
 * to ``k_ra8_wdt_clear_max_polls`` times and returns
 * ``k_ra8_err_hw_timeout`` if the bit is still set when the budget
 * runs out (the FSP analogue is the ``do/while`` loop in
 * ``R_WDT_StatusClear``).
 *
 * @param[in] mask Bit mask of flags to clear (any combination of
 * ``k_ra8_wdt_status_underflow`` / ``..._refresh``).
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Targeted flags read 0 within budget.
 * @retval k_ra8_err_invalid_arg ``mask`` outside the legal flag bits.
 * @retval k_ra8_err_hw_timeout Flag still latched after ``k_ra8_wdt_clear_max_polls``.
 *
 * @pre WDT block has been powered.
 * @pre Caller is in single-writer context (or has masked IRQs).
 *
 * @post On success, the targeted bits in WDTSR read 0.
 * @post On hw_timeout, hardware state is unchanged from a single
 * ``ra8_wdt_clear_status`` call.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_clear_status_blocking(uint16_t mask);

/**
 * @brief Read the live CNTVAL[13:0] down-counter value.
 *
 * @param[out] out_count Receives the current counter; range 0..0x3FFF.
 * The hardware notes the read may differ from
 * the actual count by 1 (HUM Ch 27.2.3 p 1261).
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Counter read.
 * @retval k_ra8_err_null_ptr ``out_count`` was null.
 *
 * @pre ``out_count`` is non-null.
 * @pre WDT block has been powered.
 *
 * @post ``*out_count`` <= 0x3FFF.
 * @post No side effect on WDTSR.
 *
 * @note Read-only; safe to call from any context.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_get_counter(uint16_t* out_count);

/* =============================================================================
 * Timeout decoding (FSP timeoutGet analogue)
 * =============================================================================
 */

/**
 * @brief Decode a ``ra8_wdt_timeout_sel_t`` into its cycle count.
 *
 * @param[in] sel TOPS field (``k_ra8_wdt_timeout_*``).
 * @param[out] out_cycles Receives the cycle count (1024 / 4096 /
 * 8192 / 16384) per HUM Ch 27.2.2 Table 27.2
 * p 1259.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Decoded.
 * @retval k_ra8_err_null_ptr ``out_cycles`` was null.
 * @retval k_ra8_err_invalid_arg ``sel`` outside 0..3.
 *
 * @pre ``out_cycles`` is non-null.
 * @pre ``sel`` is a valid TOPS encoding.
 * @post ``*out_cycles`` is one of {1024, 4096, 8192, 16384}.
 * @post No hardware state is touched.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_timeout_cycles_get(ra8_wdt_timeout_sel_t sel, uint16_t* out_cycles);

/**
 * @brief Decode a ``ra8_wdt_clock_div_t`` into its numeric divisor.
 *
 * @param[in] div CKS field (``k_ra8_wdt_clkdiv_*``).
 * @param[out] out_divisor Receives the divisor in {4, 64, 128, 512,
 * 2048, 8192} per HUM Ch 27.2.2 p 1258.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Decoded.
 * @retval k_ra8_err_null_ptr ``out_divisor`` was null.
 * @retval k_ra8_err_invalid_arg ``div`` not a legal CKS encoding.
 *
 * @pre ``out_divisor`` is non-null.
 * @pre ``div`` is one of the legal CKS encodings.
 * @post ``*out_divisor`` is one of {4, 64, 128, 512, 2048, 8192}.
 * @post No hardware state is touched.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_pclkb_divisor(ra8_wdt_clock_div_t div, uint16_t* out_divisor);

/**
 * @brief Compute the effective timeout in PCLKB cycles.
 *
 * @details
 * Returns ``cycles_for(TOPS) * divisor_for(CKS)`` as a 32-bit value.
 * Matches Table 27.2 p 1259 row by row -- e.g. CKS=PCLKB/8192,
 * TOPS=16384 yields 134_217_728 PCLKB cycles.
 *
 * @param[in] sel TOPS field.
 * @param[in] div CKS field.
 * @param[out] out_pclkb_cycles Receives the product.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Computed.
 * @retval k_ra8_err_null_ptr ``out_pclkb_cycles`` was null.
 * @retval k_ra8_err_invalid_arg ``sel`` or ``div`` invalid.
 *
 * @pre ``out_pclkb_cycles`` is non-null.
 * @pre ``sel`` and ``div`` decode successfully.
 * @post ``*out_pclkb_cycles`` <= 134_217_728 (largest table entry).
 * @post No hardware state is touched.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_total_pclkb_cycles(ra8_wdt_timeout_sel_t sel,
                                                   ra8_wdt_clock_div_t   div,
                                                   uint32_t*             out_pclkb_cycles);

/* =============================================================================
 * Single-callback API (kept for backward compatibility with the existing
 * ICU shim and the original tests; new code should use subscribe / unsubscribe)
 * =============================================================================
 */

/**
 * @brief Register a callback invoked from the WDT NMI / underflow ISR.
 *
 * @details
 * Single-callback wrapper preserved from v0.2.0 so existing wiring
 * keeps compiling. Internally just calls ``ra8_wdt_subscribe`` /
 * ``ra8_wdt_unsubscribe`` with a private "legacy" slot, so it
 * coexists with the multi-subscriber API without losing entries.
 *
 * @param[in] fn Callback fn (may be ``nullptr`` to clear).
 * @param[in] ctx Context pointer forwarded to ``fn``.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Slot updated.
 * @retval k_ra8_err_no_mem Subscriber table full and a non-null
 * ``fn`` was supplied (theoretical).
 *
 * @pre Caller is in single-writer init context.
 * @pre No NMI is being dispatched concurrently.
 *
 * @post Subsequent ``ra8_wdt_dispatch`` calls invoke ``fn``.
 * @post ``ctx`` is captured exactly as passed.
 *
 * @note Not thread-safe -- attach during init only.
 *
 * @see ra8_wdt_subscribe Multi-subscriber alternative.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_attach_handler(ra8_wdt_event_fn_t fn, void* ctx);

/* =============================================================================
 * Multi-subscriber API (.x) -- hot-pluggable underflow listeners
 * =============================================================================
 */

/**
 * @brief Add a hot-pluggable subscriber to the WDT NMI dispatch list.
 *
 * @details
 * Multiple modules can listen for the underflow / refresh-error event
 * (an SoH logger, the application crash recorder, a watchdog-aware
 * task scheduler,...). Each ``ra8_wdt_subscribe`` call grabs the
 * first free slot in the static dispatch table; ``ra8_wdt_dispatch``
 * later walks every populated slot in registration order.
 *
 * @param[in] fn Callback. Must not be ``nullptr``.
 * @param[in] ctx Caller-supplied pointer forwarded to ``fn`` on
 * every dispatch.
 * @param[out] out_slot Receives the slot index (0..k_ra8_wdt_max_subs-1)
 * assigned to this subscriber. May be ``nullptr``
 * if the caller doesn't need it.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Subscriber installed.
 * @retval k_ra8_err_null_ptr ``fn`` was null.
 * @retval k_ra8_err_no_mem All ``k_ra8_wdt_max_subs`` slots are taken.
 *
 * @pre ``fn`` is non-null.
 * @pre Caller is in single-writer init context (subscribe is not
 * NMI-safe; do all subscribes during boot).
 *
 * @post On success, exactly one previously-empty slot is filled.
 * @post Slot count grows by 1.
 *
 * @note Not thread-safe; treat the subscriber table as immutable
 * once interrupts are unmasked.
 *
 * @see ra8_wdt_unsubscribe Inverse operation.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_subscribe(ra8_wdt_event_fn_t fn, void* ctx, uint8_t* out_slot);

/**
 * @brief Remove a previously-registered subscriber.
 *
 * @param[in] slot Slot index returned by ``ra8_wdt_subscribe``.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Slot freed.
 * @retval k_ra8_err_invalid_arg ``slot`` out of range.
 * @retval k_ra8_err_not_found Slot was already empty.
 *
 * @pre ``slot`` < ``k_ra8_wdt_max_subs``.
 * @pre Caller is in single-writer init context.
 *
 * @post Slot reads ``{nullptr, nullptr}``.
 * @post Subsequent dispatches skip the slot.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_unsubscribe(uint8_t slot);

/**
 * @brief Number of currently-installed subscribers.
 *
 * @details
 * Diagnostic accessor used by tests and dump helpers. Counts only
 * slots whose ``fn`` is non-null.
 *
 * @return The current populated-slot count.
 *
 * @pre None.
 * @post No state is mutated.
 * @since 0.1.0
 *
 * @retval k_ra8_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
uint8_t ra8_wdt_subscriber_count(void);

/**
 * @brief Dispatch a WDT event -- snapshot WDTSR + fan-out to subscribers.
 *
 * @details
 * Latches the UNDFF / REFEF bits, clears them, then invokes every
 * registered subscriber (legacy ``attach_handler`` + multi-sub list)
 * in registration order. Intended to be called from the WDT NMI
 * vector. The status mask is captured *before* the W0C clear so all
 * subscribers see the same snapshot.
 *
 * @pre WDT block has been powered.
 * @pre Called from NMI context (interrupts disabled).
 *
 * @post WDTSR's UNDFF and REFEF bits will read 0 within (N + 1)
 * PCLKB cycles.
 * @post Every populated subscriber slot has been invoked exactly once.
 *
 * @note Safe to call when no subscriber is registered (becomes a flag
 * clear with no further effect).
 *
 * @since 0.1.0
 */
void ra8_wdt_dispatch(void);

/* =============================================================================
 * NMI vector wiring (HUM Ch 14.2.14 p 542)
 * =============================================================================
 */

/**
 * @brief Enable the WDT bit in NMIER so the NMI line fires on underflow.
 *
 * @details
 * HUM Ch 14.2.14 p 542 -- NMIER is sticky-set; once written it cannot
 * be cleared by software except via the matching ``ra8_wdt_uninstall_nmi``
 * helper which goes through ``ra8_icu_nmi_disable``. Use this from the
 * boot sequence after the ICU is initialized but before the WDT
 * starts decrementing.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds (the underlying ``ra8_icu_nmi_enable``
 * only OR's into the register).
 *
 * @pre ``ra8_icu_init`` has been called.
 * @pre Caller is in single-writer init context.
 *
 * @post NMIER.WDTEN reads 1.
 * @post NMICLR has cleared any stale WDT NMI status.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_install_nmi(void);

/**
 * @brief Disable the WDT bit in NMIER (counterpart to ``ra8_wdt_install_nmi``).
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre ``ra8_icu_init`` has been called.
 * @pre Caller is in single-writer context.
 *
 * @post NMIER.WDTEN reads 0.
 * @post Pending WDT NMI status is cleared via NMICLR.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_uninstall_nmi(void);

/* =============================================================================
 * Sleep-mode stop control
 * =============================================================================
 */

/**
 * @brief Enable counter halt on Sleep entry.
 *
 * @details
 * Sets WDTCSTPR.SLCSTP. In auto-start mode this is a no-op because
 * the same field is locked from OFS0.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre ``ra8_wdt_init`` has been called.
 * @pre Caller is in a single-writer context.
 *
 * @post WDTCSTPR.SLCSTP = 1 (register-start mode).
 * @post Counter halts on the next ``WFI`` / ``WFE``.
 *
 * @note Idempotent.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_enter_stop(void);

/**
 * @brief Disable counter halt on Sleep entry.
 *
 * @details
 * Clears WDTCSTPR.SLCSTP so the counter keeps decrementing in Sleep.
 * Useful when the application uses Sleep as an idle posture but still
 * wants the WDT to fire if the wake source never arrives.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre ``ra8_wdt_init`` has been called.
 * @pre Caller is in a single-writer context.
 *
 * @post WDTCSTPR.SLCSTP = 0 (register-start mode).
 * @post Counter continues to count in Sleep.
 *
 * @note Idempotent.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_exit_stop(void);

/* =============================================================================
 * OFS0 / OFS3 read-only decode (HUM Ch 27.2.6 p 1263, Table 27.5 p 1269)
 * =============================================================================
 */

/**
 * @typedef ra8_wdt_ofs_reader_fn_t
 * @brief Hook used to fetch one ``OFSm`` option-setting word.
 *
 * @details
 * Production builds default to ``internal_default_ofs_reader`` which
 * dereferences the address directly. The hook exists so unit tests
 * (and bring-up debuggers running from non-secure world) can supply
 * canned OFSm contents without needing the option-setting MRAM page
 * mapped at the host. Per CLAUDE.md NASA Rule 9 deviation, function
 * pointers are allowed for Dependency Inversion.
 *
 * It is also the seam a non-secure caller needs: ``OFS0``, ``OFS3_SEC``
 * and ``OFS3_SEL`` are all secure-region words, so a non-secure image
 * must route this through a veneer rather than loading from them.
 *
 * @param[in] ofs_addr One of the ``ra8_ofs_addr_t`` constants --
 * ``k_ra8_ofs0_addr``, ``k_ra8_ofs3_addr``, ``k_ra8_ofs3_sec_addr`` or
 * ``k_ra8_ofs3_sel_addr`` (HUM Ch 7 Figure 7.1 p 279).
 * @param[out] out_word Receives the 32-bit option-setting word.
 *
 * @return ``k_ra8_ok`` on success; ``k_ra8_err_*`` on hook-defined failure.
 */
typedef ra8_err_t (*ra8_wdt_ofs_reader_fn_t)(uintptr_t ofs_addr, uint32_t* out_word);

/**
 * @brief Override the OFSm reader hook used by ``ra8_wdt_ofs_get``.
 *
 * @details
 * Pass ``nullptr`` to restore the default reader (direct MMIO read).
 * Production code never needs to call this; it exists for unit tests
 * and for early-bring-up code that has not yet mapped the option-
 * setting page.
 *
 * @param[in] reader New reader function or ``nullptr`` for default.
 *
 * @return Always ``k_ra8_ok``.
 *
 * @pre Caller is in single-writer init context.
 * @post Subsequent ``ra8_wdt_ofs_get`` calls go through ``reader``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_ofs_reader_set(ra8_wdt_ofs_reader_fn_t reader);

/**
 * @brief Decode an OFSm option-setting word into ``ra8_wdt_cfg_t``.
 *
 * @details
 * Recovers the seven WDT fields the boot ROM latched when
 * ``OFSm.WDTnSTRT == 0``, per HUM Ch 27.3.8 Table 27.5 p 1269. The two
 * instances read different numbers of words, because the hardware does:
 *
 * - ``k_ra8_wdt0`` -- **one** read of ``OFS0`` (HUM Ch 7.2.1 p 280). OFS0 is a
 *   single secure-region word with no ``_SEC`` / ``_SEL`` companions.
 * - ``k_ra8_wdt1`` -- **three** reads. ``OFS3_SEL`` (HUM Ch 7.2.7 p 289)
 *   selects, *per field*, whether WDT1 latched that field from ``OFS3_SEC``
 *   (selector 0) or ``OFS3`` (selector 1), so the effective word is a mux of
 *   the two copies. Reading either copy alone reports a configuration the
 *   hardware may not be using.
 *
 * The WDT driver is *read-only* with respect to OFSm; only ``ra8_ofs.c``
 * may write the option-setting sections. Every fetch goes through the
 * ``ra8_wdt_ofs_reader_set`` hook so unit tests / bring-up code
 * can supply canned words without touching MRAM.
 *
 * @param[in] which Instance whose OFSm word should be decoded.
 * @param[out] out Receives the decoded view.
 *
 * @return ``ra8_err_t``
 * @retval k_ra8_ok Decoded.
 * @retval k_ra8_err_null_ptr ``out`` was null.
 * @retval k_ra8_err_invalid_arg ``which`` is not a known instance.
 * @retval k_ra8_err_invalid_state ``which`` was ``k_ra8_wdt1`` and ``OFS3_SEL``
 *         holds an encoding HUM Ch 7.2.7 p 289 marks "Setting prohibit".
 * @retval k_ra8_err_* Whatever the reader hook returned.
 *
 * @pre ``out`` is non-null.
 * @pre ``which`` < ``k_ra8_wdt_instance_count``.
 *
 * @post ``out->cfg`` mirrors the latched OFSm fields.
 * @post ``out->auto_start`` matches ``out->start_mode == k_ra8_wdt_ofs_strt_auto``.
 *
 * @note Read-only; callable from any context.
 * @warning Secure-world only under the default reader -- see
 *          ``ra8_wdt_ofs_reader_fn_t``.
 *
 * @see ra8_ofs_addr_t  The addresses this reads.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_wdt_ofs_get(ra8_wdt_instance_t which, ra8_wdt_ofs_decoded_t* out);

#ifdef __cplusplus
}
#endif
