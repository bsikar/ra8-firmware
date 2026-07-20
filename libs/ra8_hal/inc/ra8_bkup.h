/**
 * @file ra8_bkup.h
 * @brief Battery Backup Function (VBATT) HAL driver -- public API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Driver for the RA8D2 battery-backup block (HUM Ch 12, p 498-519).
 * The block has no MSTPCR bit -- it is permanently powered as long
 * as VBATT or VCC is supplied (HUM Ch 12.1 p 498). The driver exposes:
 *
 * - Lifecycle: ``ra8_bkup_init`` / ``ra8_bkup_deinit`` / cold + warm
 *   start helpers (``ra8_bkup_cold_start_init``,
 *   ``ra8_bkup_warm_start_check``, ``ra8_bkup_no_switch_init``).
 * - Status decode: ``ra8_bkup_get_status`` / ``ra8_bkup_clear_status``.
 * - Backup-register window: byte and 32-bit accessors over all 32
 *   words / 128 bytes of VBTBKRn.
 * - Tamper-detection front-end: per-channel input enable, edge
 *   select, noise-canceller enable, noise-width clock select, IRQ
 *   enable, backup-clear enable, HUK zeroize enable, RTC time-capture
 *   event source select, live input monitor.
 * - VBATT analog monitor: enable / disable + ADC-channel routing.
 * - TrustZone partitioning: BBFSAR (per-register S/NS), VBRSABAR
 *   (backup-register S/NS boundary), VBRPABAR{S,NS} (privilege
 *   boundaries inside each S/NS region).
 * - IRQ dispatch: ``ra8_bkup_attach_handler`` + ``ra8_bkup_dispatch``
 *   wired to the single VBATTADI interrupt source listed in HUM
 *   Ch 12.4 Table 12.2 p 518 (sources VBTADF0..2 share one ICU
 *   slot; the dispatch passes the live VBTADSR mask so the callback
 *   knows which channel(s) fired).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_bkup_regs.h"
#include "ra8_err.h"

/* =============================================================================
 * Configuration enums
 * =============================================================================
 */

/**
 * @enum ra8_bkup_vdet_level_t
 * @brief VDETBATT trip level (HUM Ch 12.2.12 p 508, VBTBPCR2.VDETLVL).
 *
 * @details
 * Selects the VCC threshold below which the battery power-supply
 * switch flips from VCC to VBATT. The two highest encodings (110b
 * and 111b) are listed as "setting prohibited" in the HUM and are
 * intentionally not exposed. The numeric values match the HUM
 * encoding so they can be written directly into VBTBPCR2.VDETLVL.
 */
typedef enum : uint8_t {
  k_ra8_bkup_vdet_2p80v = 0U, /**< 000b: 2.80 V (default after reset). */
  k_ra8_bkup_vdet_2p53v = 1U, /**< 001b: 2.53 V.                       */
  k_ra8_bkup_vdet_2p10v = 2U, /**< 010b: 2.10 V.                       */
  k_ra8_bkup_vdet_1p95v = 3U, /**< 011b: 1.95 V.                       */
  k_ra8_bkup_vdet_1p85v = 4U, /**< 100b: 1.85 V.                       */
  k_ra8_bkup_vdet_1p75v = 5U, /**< 101b: 1.75 V.                       */
} ra8_bkup_vdet_level_t;

/**
 * @enum ra8_bkup_source_t
 * @brief Live power-source decoded from VBTBPSR.BPWSWM.
 */
typedef enum : uint8_t {
  k_ra8_bkup_source_vbatt = 0U, /**< BPWSWM=0: VCC < VDETBATT, on VBATT. */
  k_ra8_bkup_source_vcc   = 1U, /**< BPWSWM=1: VCC > VDETBATT, on VCC.   */
} ra8_bkup_source_t;

/**
 * @enum ra8_bkup_channel_t
 * @brief RTCICn tamper-detection channel index (HUM Ch 12.2.8 p 505).
 */
typedef enum : uint8_t {
  k_ra8_bkup_chan_rtcic0 = 0U, /**< RTCIC0 -> VBTADF0 / VCH0*. */
  k_ra8_bkup_chan_rtcic1 = 1U, /**< RTCIC1 -> VBTADF1 / VCH1*. */
  k_ra8_bkup_chan_rtcic2 = 2U, /**< RTCIC2 -> VBTADF2 / VCH2*. */
} ra8_bkup_channel_t;

/**
 * @enum ra8_bkup_edge_t
 * @brief Tamper-detection edge select (HUM Ch 12.2.9 p 506).
 */
typedef enum : uint8_t {
  k_ra8_bkup_edge_falling = 0U, /**< VCHnEG = 0 -> falling edge. */
  k_ra8_bkup_edge_rising  = 1U, /**< VCHnEG = 1 -> rising edge.  */
} ra8_bkup_edge_t;

/**
 * @enum ra8_bkup_nc_width_t
 * @brief Noise-canceller sampling clock select
 *        (HUM Ch 12.2.18 p 511, VBTNCWCR.VINCW).
 *
 * @details
 * Encoding matches the HUM table -- 000b is the high-frequency
 * sub-clock, 001b..111b are the divided-down 64 Hz counter taps. When
 * VINCW is non-zero the RTC 64 Hz counter must be running.
 */
typedef enum : uint8_t {
  k_ra8_bkup_nc_width_32768hz = 0U, /**< 000b: 32.768 kHz (sub-clock). */
  k_ra8_bkup_nc_width_64hz    = 1U, /**< 001b: 64 Hz.                  */
  k_ra8_bkup_nc_width_32hz    = 2U, /**< 010b: 32 Hz.                  */
  k_ra8_bkup_nc_width_16hz    = 3U, /**< 011b: 16 Hz.                  */
  k_ra8_bkup_nc_width_8hz     = 4U, /**< 100b:  8 Hz.                  */
  k_ra8_bkup_nc_width_4hz     = 5U, /**< 101b:  4 Hz.                  */
  k_ra8_bkup_nc_width_2hz     = 6U, /**< 110b:  2 Hz.                  */
  k_ra8_bkup_nc_width_1hz     = 7U, /**< 111b:  1 Hz.                  */
} ra8_bkup_nc_width_t;

/**
 * @enum ra8_bkup_capture_src_t
 * @brief RTC time-capture event source select
 *        (HUM Ch 12.2.16 p 511, VBTADCR2.VBRTCESn).
 */
typedef enum : uint8_t {
  k_ra8_bkup_capture_src_pin    = 0U, /**< 0 -> raw RTCICn pin. */
  k_ra8_bkup_capture_src_vbtadf = 1U, /**< 1 -> VBTADFn flag.   */
} ra8_bkup_capture_src_t;

/* =============================================================================
 * Configuration / status structures
 * =============================================================================
 */

/**
 * @struct ra8_bkup_config_t
 * @brief Configuration descriptor passed to ``ra8_bkup_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; each
 * member is read in ``ra8_bkup_init`` in
 * ``libs/ra8_hal/src/ra8_bkup.c``.
 */
typedef struct {
  ra8_bkup_vdet_level_t vdet_level;    /**< VDETBATT trip threshold (VBTBPCR2.VDETLVL).         */
  bool                  enable_switch; /**< true -> clear BPWSWSTP, allow VCC->VBATT switching. */
  bool                  enable_backup; /**< true -> set VBTBER.VBAE so VBTBKRn is accessible.   */
} ra8_bkup_config_t;

/**
 * @struct ra8_bkup_status_t
 * @brief Decoded snapshot of VBTBPSR.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused; the
 * fields are written by ``ra8_bkup_get_status`` and consumed by
 * application code / tests.
 */
typedef struct {
  ra8_bkup_source_t source;       /**< Active power source right now.         */
  bool              vbatt_r_ok;   /**< VBPORM: VBATT_R > VPORBATT (post-OK).  */
  bool              por_detected; /**< VBPORF: VBATT_POR reset latched (W0C). */
  uint8_t           tamper_flags; /**< Raw VBTADSR.VBTADF[2:0] mask.          */
  uint8_t           raw_vbtbpsr;  /**< Unmodified VBTBPSR (debug aid).        */
} ra8_bkup_status_t;

/**
 * @struct ra8_bkup_tamper_chan_cfg_t
 * @brief Per-channel tamper-detection configuration.
 *
 * @details
 * One descriptor per RTCICn channel passed to
 * ``ra8_bkup_tamper_init`` to wire up VBTICTLR / VBTICTLR2 /
 * VBTADCR1..3 / VBTADCR2 in one shot. cppcheck cannot see tests/ so
 * it flags every field as unused.
 */
typedef struct {
  bool                   input_enable;       /**< VCHnINEN: drive RTCICn pad as input.       */
  bool                   noise_canceller_en; /**< VCHnNCE: enable 3-tap synchroniser.        */
  ra8_bkup_edge_t        edge;               /**< VCHnEG: falling vs rising trigger edge.    */
  bool                   irq_enable;         /**< VBTADIEn: route flag to VBATTADI ICU slot. */
  bool                   clear_backup;       /**< VBTADCEn: zeroise VBTBKRn on flag.         */
  bool                   zeroize_huk;        /**< VBTADZEn: assert HUK zeroize on flag.      */
  ra8_bkup_capture_src_t capture_src;        /**< VBRTCESn: RTC time-capture source select.  */
} ra8_bkup_tamper_chan_cfg_t;

/**
 * @struct ra8_bkup_tamper_config_t
 * @brief Whole-block tamper-detection configuration.
 *
 * @details
 * Aggregates the noise-canceller width and per-channel descriptors so
 * one call to ``ra8_bkup_tamper_init`` covers HUM Ch 12.3.7.4
 * "Tamper Detection Function Initialization Setting Flow" steps 1-8.
 * cppcheck cannot see tests/ so it flags every field as unused.
 */
typedef struct {
  ra8_bkup_nc_width_t        nc_width; /**< VINCW noise-sampling clock. */
  ra8_bkup_tamper_chan_cfg_t channels[(uint8_t)k_ra8_bkup_chan_count]; /**< Channels. */
} ra8_bkup_tamper_config_t;

/**
 * @struct ra8_bkup_security_config_t
 * @brief BBFSAR + backup-register boundary descriptor.
 *
 * @details
 * Drives ``ra8_bkup_security_apply``. ``saba``, ``pabas`` and
 * ``pabans`` are 16-bit lower-half addresses (HUM Ch 12.2.2/3/4 say
 * the boundary is in the SYSC base ``0x4001_0000`` plus the
 * register value, in 32-byte units). ``bbfsar`` is the per-register
 * S/NS bitmap (HUM Ch 12.2.1 NONSEC0..NONSEC6).
 *
 * cppcheck cannot see tests/ so it flags every field as unused.
 */
typedef struct {
  uint32_t bbfsar; /**< OR of ``k_ra8_bkup_bbfsar_mask_*`` bits. */
  uint16_t saba;   /**< VBRSABAR.SABA (32-byte aligned).         */
  uint16_t pabas;  /**< VBRPABARS.PABAS (32-byte aligned).       */
  uint16_t pabans; /**< VBRPABARNS.PABANS (32-byte aligned).     */
} ra8_bkup_security_config_t;

/**
 * @typedef ra8_bkup_event_fn_t
 * @brief Low-battery / tamper event callback signature.
 *
 * @param[in] ctx          Caller context attached at registration.
 * @param[in] tamper_flags Raw VBTADSR.VBTADF[2:0] mask at the moment
 *                         the ISR dispatched. 0 means a non-tamper
 *                         event (low-battery / VBATT_POR).
 */
typedef void (*ra8_bkup_event_fn_t)(void* ctx, uint8_t tamper_flags);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Configure the battery-backup block from ``cfg``.
 *
 * @details
 * Sequence (HUM Ch 12.2.11 p 507 + Ch 12.2.6 p 504):
 * 1. If ``cfg->enable_switch``, clear BPWSWSTP and program
 *    VBTBPCR2.VDETLVL, then set VDETE so the VCC drop detector arms.
 * 2. If ``cfg->enable_backup``, write 1 to VBTBER.VBAE to open the
 *    VBTBKRn access window, then busy-wait the HUM-mandated >= 500 ns
 *    settle (HUM Ch 12.2.6 p 504: "You must write 1 to VBAE before
 *    accessing VBTBKR" ... "wait for at least 500 ns after writing 1 to
 *    VBAE, and then access VBTBKR"). VBAE resets to 1, but the HUM
 *    procedure requires the explicit write + settle. To *retain* VBTBKRn
 *    across a real VBATT cutover the app must later write VBAE back to 0
 *    via ``ra8_bkup_deinit``. Note: VBAE is only one precondition -- the
 *    battery-backup block is also inoperative unless voltage monitor 0
 *    (LVD0) reset is enabled via the ``OFS1.PVDAS`` option byte (HUM
 *    Ch 12.1.3 p 499, Ch 12.3.2 p 514), which is a boot-time option
 *    setting outside this driver's scope.
 * 3. Clear any latched VBPORF and tamper flags so the first call to
 *    ``ra8_bkup_get_status`` starts from a known state.
 *
 * The driver does not unlock PRCR -- the caller (typically board
 * init) is expected to handle the SYSC protection key for the few
 * BAT* registers that require it.
 *
 * @par State Machine
 * Driver-state transitions enforced by this entry point:
 * @dot
 * digraph ra8_bkup_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Off [label="Off"];
 *   Armed [label="Armed"];
 *
 *   __start -> Off;
 *   Off -> Armed [label="ra8_bkup_init(cfg)"];
 *   Armed -> Off [label="ra8_bkup_deinit()"];
 * }
 * @enddot
 *
 * @param[in] cfg Non-NULL configuration descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Hardware armed; backup window open.
 * @retval k_ra8_err_null_ptr   ``cfg == nullptr``.
 * @retval k_ra8_err_invalid_arg ``cfg->vdet_level`` is one of the two
 *                               "setting prohibited" encodings.
 *
 * @pre PRCR group 0 is unlocked (caller's responsibility).
 * @pre IRQs masked or single-threaded init context.
 * @post BPWSWSTP and VBAE reflect ``cfg``.
 * @post When ``cfg->enable_backup``, the VBTBKRn window is armed and the
 *       >= 500 ns settle has elapsed, so the next VBTBKRn access is valid.
 * @post VBTADSR == 0 and VBPORF == 0.
 *
 * @note Not thread-safe.
 *
 * @see ra8_bkup_deinit
 * @see ra8_bkup_cold_start_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_init(const ra8_bkup_config_t* cfg);

/**
 * @brief Disable the battery-backup switch and close the VBTBKRn window.
 *
 * @details
 * Clears VBAE so VBTBKRn data is preserved across VCC loss (per HUM
 * Ch 12.2.6 p 504 the bit must be 0 before VBATT cutover) and stops
 * the battery power-supply switch by setting BPWSWSTP. Leaves the
 * latched VBPORF / tamper flags alone -- use
 * ``ra8_bkup_clear_status`` to also wipe them.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre Driver previously initialized via ``ra8_bkup_init``.
 * @pre IRQs masked or single-threaded shutdown context.
 * @post VBTBER.VBAE == 0.
 * @post VBTBPCR1.BPWSWSTP == 1.
 *
 * @note Not thread-safe.
 *
 * @see ra8_bkup_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_deinit(void);

/**
 * @brief Drive the cold-start flow from HUM Ch 12.3.7.1 p 517.
 *
 * @details
 * Implements the seven HUM-documented steps for first-time power-up of
 * both VCC and VBATT:
 *  1. Wait for VBPORM to read 1 (VBATT_R above VPORBATT).
 *  2. Clear VBPORF.
 *  3. Program VDETLVL.
 *  4. Caller must wait tDETWT externally (see ``timeout_iters``).
 *  5. Set VDETE.
 *  6. Sub-clock + RTC bring-up is the caller's job (out of scope here).
 *  7. Returns success once the switch is armed.
 *
 * The wait in step 1 is bounded by ``timeout_iters`` polling cycles to
 * avoid livelock on broken hardware -- callers should pass a value
 * that comfortably exceeds the spec'd VBATT_R rise time.
 *
 * @param[in] level         VDETBATT level to programme.
 * @param[in] timeout_iters Maximum poll iterations waiting for VBPORM.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Switch armed.
 * @retval k_ra8_err_invalid_arg ``level`` out of range.
 * @retval k_ra8_err_hw_timeout  VBPORM stayed 0 for the whole poll loop.
 *
 * @pre PRCR unlocked, IRQs masked.
 * @pre ``timeout_iters > 0``.
 * @post VBTBPCR2.VDETE == 1.
 * @post VBPORF cleared, VDETLVL == ``level``.
 *
 * @note Caller still needs to wait tDETWT between this call and any
 *       use of the switch; the wait is intentionally external so the
 *       HAL stays clock-agnostic.
 *
 * @see ra8_bkup_warm_start_check
 * @see ra8_bkup_no_switch_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_cold_start_init(ra8_bkup_vdet_level_t level,
                                                 uint32_t              timeout_iters);

/**
 * @brief Warm-start sanity check (HUM Ch 12.3.7.2 p 517).
 *
 * @details
 * After a VBATT->VCC transition the firmware must:
 *  1. Wait for VBPORM == 1.
 *  2. Inspect VBPORF: if set, the VBATT_R rail dropped below
 *     VPORBATT and the entire backup area must be reinitialized by
 *     calling ``ra8_bkup_cold_start_init``. If clear, the backup area
 *     state survived and no further action is required.
 *
 * @param[out] needs_reinit Set to ``true`` when VBPORF was latched
 *                          and the caller must run cold-start init.
 * @param[in]  timeout_iters Max polls waiting for VBPORM.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            Status decoded into ``*needs_reinit``.
 * @retval k_ra8_err_null_ptr  ``needs_reinit == nullptr``.
 * @retval k_ra8_err_hw_timeout VBPORM stayed 0.
 *
 * @pre PRCR unlocked.
 * @pre ``timeout_iters > 0``.
 * @post ``*needs_reinit`` reflects VBPORF.
 * @post VBPORF is left untouched (caller decides when to W0C it).
 *
 * @note Pure status read; safe to call from cold reset path.
 *
 * @see ra8_bkup_cold_start_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_warm_start_check(bool* needs_reinit, uint32_t timeout_iters);

/**
 * @brief "Power-supply switch unused" init flow (HUM Ch 12.3.7.3 p 518).
 *
 * @details
 * Used when VCC and VBATT are externally tied together. Sets
 * BPWSWSTP, waits for VBPORM to drop, clears VDETE, restores
 * VDETLVL to the prohibited 110b sentinel (HUM-documented "initial"
 * value), W0Cs VBPORF, and zeroes VBTICTLR / VBTICTLR2 / VBTADSR /
 * VBTADCR1 / VBTADCR2 / VBTBKRn. Sub-clock and RTC bring-up are out
 * of scope for the HAL.
 *
 * @param[in] timeout_iters Max polls waiting for VBPORM == 0.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All registers reset, switch stopped.
 * @retval k_ra8_err_hw_timeout  VBPORM never dropped.
 *
 * @pre PRCR unlocked, IRQs masked.
 * @pre ``timeout_iters > 0``.
 * @post VBTBPCR1.BPWSWSTP == 1.
 * @post VBTBPCR2.VDETE == 0 and VDETLVL == 110b.
 * @post VBTICTLR / VBTICTLR2 / VBTADSR / VBTADCR1 / VBTADCR2 == 0.
 *
 * @see ra8_bkup_cold_start_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_no_switch_init(uint32_t timeout_iters);

/* =============================================================================
 * Status
 * =============================================================================
 */

/**
 * @brief Read VBTBPSR + VBTADSR and decode into ``out``.
 *
 * @param[out] out Non-NULL status descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok            ``out`` populated.
 * @retval k_ra8_err_null_ptr  ``out == nullptr``.
 *
 * @pre Driver initialized.
 * @pre ``out`` points to writable storage.
 * @post ``out->source`` reflects current BPWSWM bit.
 * @post ``out->raw_vbtbpsr`` matches the live register read.
 *
 * @note Thread safety: read-only, safe to call concurrently with
 *       writes provided the register is naturally aligned (it is).
 *
 * @see ra8_bkup_clear_status
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_get_status(ra8_bkup_status_t* out);

/**
 * @brief Clear latched VBPORF and tamper flags selected by ``mask``.
 *
 * @details
 * VBPORF is W0C (write-0-to-clear) per HUM Ch 12.2.13 p 509; the
 * driver does the inverted-mask write so callers can pass the same
 * bit mask they observed via ``ra8_bkup_get_status``.
 *
 * @param[in] mask  OR of ``k_ra8_bkup_vbtbpsr_mask_vbporf`` and
 *                  ``k_ra8_bkup_vbtadsr_mask_*`` values.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds (bits not set in ``mask`` left alone).
 *
 * @pre Driver initialized.
 * @pre IRQs masked or single-threaded context.
 * @post VBPORF cleared if requested.
 * @post Bits of VBTADSR present in ``mask`` cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_clear_status(uint8_t mask);

/* =============================================================================
 * VBTBKRn 32-bit word access
 * =============================================================================
 */

/**
 * @brief Read one 32-bit word from the VBTBKRn array.
 *
 * @param[in]  word_index Word index in 0..31 (32 words = 128 bytes).
 * @param[out] out        Non-NULL receiver for the word value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok             Word read into ``*out``.
 * @retval k_ra8_err_null_ptr   ``out == nullptr``.
 * @retval k_ra8_err_invalid_arg ``word_index >= 32``.
 *
 * @pre Driver initialized with ``cfg->enable_backup == true``.
 * @pre ``out`` points to writable storage.
 * @post ``*out`` matches the live 32-bit register value.
 *
 * @note Thread safety: read-only.
 *
 * @see ra8_bkup_write_word
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_read_word(uint8_t word_index, uint32_t* out);

/**
 * @brief Write one 32-bit word into the VBTBKRn array.
 *
 * @param[in] word_index Word index in 0..31.
 * @param[in] value      32-bit value to store.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Value written.
 * @retval k_ra8_err_invalid_arg ``word_index >= 32``.
 *
 * @pre Driver initialized with ``cfg->enable_backup == true``.
 * @pre IRQs masked or single-threaded context.
 * @post VBTBKRn[word_index*4 .. word_index*4+3] == ``value`` (LE).
 *
 * @note Not thread-safe.
 *
 * @see ra8_bkup_read_word
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_write_word(uint8_t word_index, uint32_t value);

/**
 * @brief Read one 8-bit byte from the VBTBKRn array.
 *
 * @param[in]  index Backup-register index in 0..127.
 * @param[out] out   Non-NULL receiver.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Byte read.
 * @retval k_ra8_err_null_ptr    ``out == nullptr``.
 * @retval k_ra8_err_invalid_arg ``index >= 128``.
 *
 * @pre Driver initialized with ``cfg->enable_backup == true``.
 * @pre ``out`` writable.
 * @post ``*out`` matches the live byte.
 *
 * @see ra8_bkup_write_byte
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_read_byte(uint16_t index, uint8_t* out);

/**
 * @brief Write one 8-bit byte into the VBTBKRn array.
 *
 * @param[in] index Backup-register index in 0..127.
 * @param[in] value Byte to store.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Byte written.
 * @retval k_ra8_err_invalid_arg ``index >= 128``.
 *
 * @pre Driver initialized with ``cfg->enable_backup == true``.
 * @post VBTBKRn[index] == ``value``.
 *
 * @see ra8_bkup_read_byte
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_write_byte(uint16_t index, uint8_t value);

/**
 * @brief Bulk-zeroise the entire 128-byte backup-register array.
 *
 * @details
 * Software equivalent of the tamper-detection backup-clear path.
 * Useful when an application detects a logical tamper that does not
 * trip an RTCICn pin.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All 128 VBTBKRn slots zeroed.
 *
 * @pre Driver initialized with ``cfg->enable_backup == true``.
 * @pre IRQs masked.
 * @post Every VBTBKRn[0..127] == 0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_zero_all(void);

/* =============================================================================
 * Tamper-detection / RTCIC pad wiring
 * =============================================================================
 */

/**
 * @brief Apply per-channel tamper configuration in one shot.
 *
 * @details
 * Implements HUM Ch 12.3.7.4 p 518 steps 1-8: programs VCHnINEN /
 * VCHnNCE / VCHnEG, sets VINCW for the noise canceller, dummy-reads
 * and clears VBTADFn flags after edge programming, then enables
 * VBTADCR1 (IRQ + clear) / VBTADCR2 (capture source) / VBTADCR3
 * (HUK zeroize) per channel.
 *
 * The HUM-documented 50us pin-stable wait and 5-RTC-clock noise
 * canceller wait are the caller's responsibility -- the HAL has no
 * generic delay primitive.
 *
 * @param[in] cfg Non-NULL whole-block configuration.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Hardware programmed.
 * @retval k_ra8_err_null_ptr    ``cfg == nullptr``.
 * @retval k_ra8_err_invalid_arg ``cfg->nc_width`` out of range.
 *
 * @pre PRCR unlocked, IRQs masked.
 * @pre All ``cfg->channels[].edge`` are valid enum values.
 * @post VBTICTLR / VBTICTLR2 / VBTNCWCR / VBTADCR1..3 reflect ``cfg``.
 * @post VBTADSR cleared after the dummy-read step.
 *
 * @see ra8_bkup_tamper_disable
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_tamper_init(const ra8_bkup_tamper_config_t* cfg);

/**
 * @brief Disable every tamper input and clear flags.
 *
 * @details
 * Counterpart to ``ra8_bkup_tamper_init``: zeroes VBTICTLR /
 * VBTICTLR2 / VBTADCR1 / VBTADCR2 / VBTADCR3 and W0Cs VBTADSR.
 * Used during deinit and during the noise-canceller reconfiguration
 * dance the HUM mandates ("VCHnNCE, VBTADCR1/2/3 should be disabled
 * before changing VINCW", HUM Ch 12.3.5 p 516).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Hardware disarmed.
 *
 * @pre PRCR unlocked, IRQs masked.
 * @post VBTICTLR / VBTICTLR2 / VBTADCR1 / VBTADCR2 / VBTADCR3 == 0.
 * @post VBTADSR.VBTADF[2:0] cleared.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_tamper_disable(void);

/**
 * @brief Read the live RTCICn pin level via VBTIMONR.
 *
 * @param[in]  channel  RTCICn channel index.
 * @param[out] high_out Set to ``true`` if the pin reads high.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Level decoded.
 * @retval k_ra8_err_null_ptr    ``high_out == nullptr``.
 * @retval k_ra8_err_invalid_arg ``channel >= 3``.
 *
 * @pre Driver initialized; VCHnINEN must be 1 for the read to be
 *      meaningful (HUM Ch 12.2.10 p 506 -- otherwise reads as 0).
 * @post ``*high_out`` matches the live VCHnMON bit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_read_input(ra8_bkup_channel_t channel, bool* high_out);

/**
 * @brief Convenience wrapper that just sets / clears VCHnINEN.
 *
 * @param[in] channel RTCICn channel index.
 * @param[in] enable  ``true`` -> drive pad as input.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              VBTICTLR updated.
 * @retval k_ra8_err_invalid_arg ``channel >= 3``.
 *
 * @pre PRCR unlocked.
 * @post Bit ``channel`` of VBTICTLR matches ``enable``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_set_input_enable(ra8_bkup_channel_t channel, bool enable);

/* =============================================================================
 * VBATT analog voltage monitor (VBATTMNSELR)
 * =============================================================================
 */

/**
 * @brief Enable / disable the VBATT/6 analog tap to ADC16H.
 *
 * @details
 * HUM Ch 12.2.5 p 503 + Ch 12.3.6 p 517: set VBTMNSEL = 1 to route
 * VBATT/6 to ANVBAT (ADC channel input). The HUM mandates a
 * tMONWT settling delay before the ADC reading is valid; the wait is
 * the caller's responsibility because the HAL has no delay primitive.
 * Recommend clearing again as soon as the conversion completes
 * because VBTMNSEL = 1 increases VBATT current draw.
 *
 * @param[in] enable ``true`` -> enable monitor; ``false`` -> disable.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Always succeeds.
 *
 * @pre PRCR unlocked.
 * @post VBATTMNSELR.VBTMNSEL == ``enable``.
 *
 * @see ra8_bkup_get_voltage_monitor_enabled
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_set_voltage_monitor(bool enable);

/**
 * @brief Read the live VBATTMNSELR.VBTMNSEL bit.
 *
 * @param[out] enabled_out Set to ``true`` when the analog tap is on.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Status decoded.
 * @retval k_ra8_err_null_ptr ``enabled_out == nullptr``.
 *
 * @pre ``enabled_out`` writable.
 * @post ``*enabled_out`` matches the register bit.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_get_voltage_monitor_enabled(bool* enabled_out);

/* =============================================================================
 * TrustZone partitioning
 * =============================================================================
 */

/**
 * @brief Apply BBFSAR + VBRSABAR + VBRPABAR{S,NS} from one descriptor.
 *
 * @details
 * Validates that ``saba`` / ``pabas`` / ``pabans`` are 32-byte
 * aligned (HUM Ch 12.2.2/3/4 require the bottom 5 bits to be 0) and
 * within ``k_ra8_bkup_saba_max``, then writes the registers in the
 * order required by Renesas SDK convention -- BBFSAR first (so the
 * NS view sees the right registers), then VBRSABAR (S/NS split),
 * then VBRPABARS / VBRPABARNS (privilege split inside each region).
 *
 * @param[in] cfg Non-NULL security descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              All four registers updated.
 * @retval k_ra8_err_null_ptr    ``cfg == nullptr``.
 * @retval k_ra8_err_invalid_arg An address is unaligned or > saba_max,
 *                              or ``bbfsar`` has reserved bits set.
 *
 * @pre PRCR unlocked (BBFSAR is PRCR-protected per HUM Ch 12.2.1).
 * @pre IRQs masked or boot context.
 * @post BBFSAR == cfg->bbfsar.
 * @post VBRSABAR / VBRPABARS / VBRPABARNS reflect ``cfg``.
 *
 * @see ra8_bkup_security_get
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_security_apply(const ra8_bkup_security_config_t* cfg);

/**
 * @brief Read back the four security partition registers.
 *
 * @param[out] cfg Non-NULL receiver.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok           Snapshot returned.
 * @retval k_ra8_err_null_ptr ``cfg == nullptr``.
 *
 * @pre ``cfg`` writable.
 * @post ``cfg`` mirrors the live registers.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_security_get(ra8_bkup_security_config_t* cfg);

/* =============================================================================
 * Interrupt path
 * =============================================================================
 */

/**
 * @brief Attach the shared low-battery / tamper event callback.
 *
 * @details
 * The battery-backup block exposes its tamper / low-battery events
 * through ICU IELSR slots that map onto VBTADSR.VBTADF[2:0]. The
 * driver provides a single callback slot; the ISR is expected to
 * call ``ra8_bkup_dispatch`` with the live VBTADSR mask.
 *
 * @param[in] fn  Callback function (may be ``nullptr`` to detach).
 * @param[in] ctx Opaque pointer forwarded to the callback.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Handler stored.
 *
 * @pre Driver initialized.
 * @pre Caller-managed lifetime for ``ctx``.
 * @post Subsequent ``ra8_bkup_dispatch`` invokes ``fn``.
 * @post Passing ``nullptr`` for ``fn`` makes ``dispatch`` a no-op.
 *
 * @note Thread safety: callback storage is plain pointer assignment,
 *       not atomic.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_attach_handler(ra8_bkup_event_fn_t fn, void* ctx);

/**
 * @brief ISR entry point that fires the registered callback.
 *
 * @details
 * Reads VBTADSR, masks against VBTADCR1.VBTADIE[2:0] (only IRQ-armed
 * channels generate the VBATTADI interrupt per HUM Ch 12.4 Table 12.2
 * p 518), W0Cs the flags it dispatched on, then invokes the
 * registered callback with the mask of fired channels.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok                Dispatched (or no-op if no flag set).
 * @retval k_ra8_err_not_initialized Driver not initialized.
 *
 * @pre ISR caller, IRQs masked at this point.
 * @post Flagged-and-armed VBTADFn bits are W0Ced.
 * @post Callback invoked exactly once with the dispatched mask.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_bkup_isr_handle(void);

/**
 * @brief Fire the registered callback (called from the ISR shim).
 *
 * @param[in] tamper_flags Snapshot of VBTADSR observed in the ISR.
 *
 * @pre Driver initialized.
 * @pre Caller has read VBTADSR before clearing flags.
 * @post Callback executed if attached.
 * @post No state change in this driver.
 *
 * @note Thread safety: not re-entrant; call from one ISR context only.
 * @since 0.1.0
 *
 * @details See the matching header declaration for the full
 * contract; this site adds no behaviour beyond what the public
 * API documents.
 */
void ra8_bkup_dispatch(uint8_t tamper_flags);

#ifdef __cplusplus
}
#endif
