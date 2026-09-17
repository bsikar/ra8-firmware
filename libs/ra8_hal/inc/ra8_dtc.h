/**
 * @file ra8_dtc.h
 * @brief Data Transfer Controller (DTC) driver
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Driver for the RA8D2 DTC block. The DTC is a lighter-weight
 * alternative to the DMAC for moving small amounts of data in response
 * to peripheral interrupts. It shares the MSTPA22 gate with DMAC0 via
 * ra8_mstp's reference counter. The driver owns the DTCCR / DTCVBR /
 * DTCST / DTCSTS surface and exposes a shared activation-callback
 * slot that the ICU dispatcher forwards DTC completion events into.
 *
 * FSP transfer-API mapping (see `r_dtc.c`):
 *  - `R_DTC_Open`        -> @ref ra8_dtc_init (programme DTCVBR + MSTP)
 *  - `R_DTC_Close`       -> @ref ra8_dtc_deinit
 *  - `R_DTC_Enable`      -> @ref ra8_dtc_enable
 *  - `R_DTC_Disable`     -> @ref ra8_dtc_disable
 *  - `R_DTC_Reconfigure` -> @ref ra8_dtc_reconfigure
 *  - `R_DTC_CallbackSet` -> @ref ra8_dtc_attach_handler
 *  - `R_DTC_Reset`, `_InfoGet`: call sites edit the TI table directly.
 *  - `R_DTC_Reload`, `_SoftwareStart`, `_SoftwareStop`: not supported
 *    (matches FSP `FSP_ERR_UNSUPPORTED`).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dtc_regs.h"
#include "ra8_err.h"

/**
 * @typedef ra8_dtc_event_fn_t
 * @brief DTC activation-complete callback.
 */
typedef void (*ra8_dtc_event_fn_t)(void* ctx, uint16_t status);

/**
 * @brief Initialise the DTC and install its vector table base.
 *
 * @param[in] vector_base Pointer to a caller-supplied DTC vector
 * table (16-byte-aligned SRAM region).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_init(void* vector_base);

/**
 * @brief Tear down the DTC (clears run bit + drops MSTP ref).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_deinit(void);

/**
 * @brief Start the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_enable(void);

/**
 * @brief Stop the DTC module.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_disable(void);

/**
 * @brief Reconfigure the DTC vector base at runtime.
 *
 * @details
 * Safe only while the DTC is disabled. Rewrites DTCVBR + toggles
 * DTCCR.RRS so that a stale read-skip entry does not outlive the
 * reprogramming.
 *
 * @param[in] vector_base New vector table base (16-byte-aligned).
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_reconfigure(void* vector_base);

/**
 * @brief Read the DTCSTS activation status register.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_get_status(uint16_t* out_mask);

/**
 * @brief Clear sticky bits in DTCSTS.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_clear_status(uint16_t mask);

/**
 * @brief Attach a shared activation-complete callback.
 *
 * @details
 * DTC activation events land on per-source IRQs via the ICU; the
 * ICU dispatcher calls ra8_dtc_dispatch() which fans them out to
 * the handler stored here.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_attach_handler(ra8_dtc_event_fn_t fn, void* ctx);

/**
 * @brief Fire the attached activation callback with current DTCSTS.
 *
 * @details
 * Reads ``DTC.DTCSTS`` (HUM Ch 17.2.10 "DTCSTS : DTC Status Register",
 * p 654) which captures the most recent DTC activation source and ACT
 * flag, and invokes the handler registered through
 * ``ra8_dtc_attach_handler()`` with that status word and the stored
 * context pointer. Silently returns when no handler is installed.
 *
 * @pre ``ra8_dtc_init()`` previously succeeded.
 * @pre Called from ISR context or unit-test driver.
 * @post Registered handler invoked at most once with current ``DTCSTS``.
 * @post No DTC register state mutated by the dispatch itself.
 *
 * @note Thread safety: ISR context only; not re-entrant.
 * @since 0.1.0
 */
void ra8_dtc_dispatch(void);

/* ---------------------------------------------------------------------
 * Transfer-descriptor facade (issue #774)
 *
 * ra8_dtc_init()/_enable()/_reconfigure() cover the module; describing a
 * transfer was left to the consumer, so three apps each transcribed the
 * MRA/MRB field encoding, the TI and vector-table alignments, the
 * "CRA = 0x0000 means 256" quirk and the IELSR.DTCE write into
 * application code. The two calls below carry those facts instead.
 * ------------------------------------------------------------------ */

/**
 * @enum ra8_dtc_unit_t
 * @brief Transfer unit width (MRA.SZ, HUM Ch 18.2.2 p 786).
 */
typedef enum : uint8_t {
  k_ra8_dtc_unit_byte = 0x0U, /**< SZ = 00b: 8-bit units.  */
  k_ra8_dtc_unit_half = 0x1U, /**< SZ = 01b: 16-bit units. */
  k_ra8_dtc_unit_word = 0x2U, /**< SZ = 10b: 32-bit units. */
} ra8_dtc_unit_t;

/**
 * @enum ra8_dtc_addr_mode_t
 * @brief Source / destination address behaviour (MRA.SM, MRB.DM).
 *
 * @details
 * HUM Ch 18.2.2 p 786 (MRA.SM[3:2]) and 18.2.3 p 787 (MRB.DM[3:2]).
 * Only the two modes the tree actually encodes are exposed: fixed and
 * increment. Decrement and the DTCDISP offset-addition mode are
 * deliberately absent -- nothing in-tree encodes them and
 * `tools/ra8_emulator`'s DTC model steps addresses only for the
 * increment code, so an app selecting them would get silent
 * fixed-address behaviour under the emulator. Follow-up slice.
 */
typedef enum : uint8_t {
  k_ra8_dtc_addr_fixed = 0x0U, /**< SM/DM = 00b: address held fixed. */
  k_ra8_dtc_addr_inc   = 0x2U, /**< SM/DM = 10b: address incremented. */
} ra8_dtc_addr_mode_t;

/**
 * @enum ra8_dtc_mode_t
 * @brief Transfer mode (MRA.MD, HUM Ch 18.2.2 p 786).
 *
 * @details
 * Repeat mode (MRA.MD = 01b) is not exposed in this slice: no in-tree
 * consumer encodes it and the emulator's transfer engine models only the
 * block and non-block paths. Follow-up slice.
 */
typedef enum : uint8_t {
  k_ra8_dtc_mode_normal = 0x0U, /**< MD = 00b: normal transfer. */
  k_ra8_dtc_mode_block  = 0x2U, /**< MD = 10b: block transfer.  */
} ra8_dtc_mode_t;

/**
 * @enum ra8_dtc_facade_limit_t
 * @brief Descriptor-facade limits.
 */
typedef enum : uint16_t {
  k_ra8_dtc_block_units_max = 256U, /**< Block size ceiling; 256 encodes as CRA = 0x0000. */
  k_ra8_dtc_vector_entries  = 96U,  /**< Vector-table entries, one per IELSR slot.        */
} ra8_dtc_facade_limit_t;

/**
 * @struct ra8_dtc_ti_t
 * @brief One correctly-aligned Transfer Information block.
 *
 * @details
 * Caller-owned storage that carries its own 16-byte alignment (HUM
 * Ch 18.3.1 p 796), so a consumer declares `static ra8_dtc_ti_t s_ti;`
 * instead of repeating a `[[gnu::aligned]]` attribute it had to read the
 * hardware manual to justify.
 */
typedef struct {
  alignas(k_ra8_dtc_vector_align) r_dtc_xfer_info_t ti; /**< The 16-byte TI block. */
} ra8_dtc_ti_t;

/**
 * @struct ra8_dtc_vector_table_t
 * @brief One correctly-aligned, correctly-sized DTC vector table.
 *
 * @details
 * `DTCVBR`'s low ten bits must be zero (HUM Ch 18.2.2 p 787), and the
 * table holds one 4-byte TI start address per ICU IELSR slot (HUM
 * Ch 18.3.1 p 796, Figure 18.3 p 798). Pass `table.entry` to
 * @ref ra8_dtc_init.
 */
typedef struct {
  alignas(k_ra8_dtc_vector_table_align)
    uint32_t entry[k_ra8_dtc_vector_entries]; /**< TI start address per slot. */
} ra8_dtc_vector_table_t;

/**
 * @struct ra8_dtc_xfer_cfg_t
 * @brief The transfer a consumer wants, in consumer terms.
 *
 * @details
 * Modelled on `ra8_dmac_start_block()`'s argument shape. Counts are in
 * transfer units, not register encodings:
 *  - block mode: @p unit_count is the block size in units (1..256, where
 *    256 is encoded as CRA = 0x0000 per HUM Ch 18.2.7 p 790) and
 *    @p block_count is the number of blocks (CRB, HUM Ch 18.2.8 p 791).
 *  - normal mode: @p unit_count is the transfer count (CRA) and
 *    @p block_count must be 0.
 */
typedef struct {
  const void*         src;         /**< Source address (SAR).                    */
  void*               dst;         /**< Destination address (DAR).               */
  ra8_dtc_addr_mode_t src_mode;    /**< Source address behaviour (MRA.SM).       */
  ra8_dtc_addr_mode_t dst_mode;    /**< Destination address behaviour (MRB.DM).  */
  ra8_dtc_unit_t      unit;        /**< Unit width (MRA.SZ).                     */
  ra8_dtc_mode_t      mode;        /**< Transfer mode (MRA.MD).                  */
  uint16_t            unit_count;  /**< Units per transfer / per block (CRA).    */
  uint16_t            block_count; /**< Blocks (CRB); 0 outside block mode.      */
} ra8_dtc_xfer_cfg_t;

/**
 * @brief Encode a transfer description into a Transfer Information block.
 *
 * @details
 * Pure function: writes MR (MRA/MRB), SAR, DAR, CRA and CRB into
 * @p out_ti and touches no hardware, so the field encoding is testable
 * off-target. MRC is left zero (no chained transfer).
 *
 * @param[in]  cfg    Transfer to encode.
 * @param[out] out_ti Caller-owned TI block to fill.
 * @return `k_ra8_ok`, or an error describing what was rejected.
 * @retval k_ra8_err_null_ptr    @p cfg, @p out_ti, `cfg->src` or `cfg->dst` was NULL.
 * @retval k_ra8_err_invalid_arg Unknown enum value, or a count the mode disallows
 *                               (block size 0 or > 256, zero block count in block
 *                               mode, zero transfer count, non-zero block count
 *                               outside block mode).
 * @post On success @p out_ti describes exactly @p cfg; on failure it is untouched.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_describe(const ra8_dtc_xfer_cfg_t* cfg, ra8_dtc_ti_t* out_ti);

/**
 * @brief Describe a transfer and arm an ICU slot to activate it.
 *
 * @details
 * The descriptor half of the driver, modelled on `ra8_dmac_start_block()`:
 *  1. encodes @p cfg into @p ti (see @ref ra8_dtc_describe),
 *  2. points the vector-table slot for @p icu_slot at that TI block
 *     (`DTCVBR + icu_slot*4`, HUM Ch 18.3.1 p 796) using the base
 *     retained by @ref ra8_dtc_init,
 *  3. cleans the D-cache over both things it just wrote, the TI block and
 *     the vector table, and
 *  4. sets `ICU.IELSRn.DTCE` for @p icu_slot through `ra8_isr_set_dtc()`
 *     (HUM Ch 14.2.17 p 547).
 *
 * The IELSR slot index doubles as the DTC vector number, so @p icu_slot is
 * the slot `ra8_isr_register()` handed back for the activation source.
 *
 * @warning The DTC is direction-blind, like the DMAC: this call cleans the
 * descriptor it wrote, not the payload. The caller still cleans its own
 * source buffer before activation and invalidates its destination
 * afterwards when the D-cache is on.
 *
 * @param[in]     icu_slot IELSR slot / DTC vector number to arm.
 * @param[in]     cfg      Transfer to describe.
 * @param[in,out] ti       Caller-owned TI block; rewritten on every call
 *                         (the DTC consumes SAR/DAR/CRA/CRB in place when
 *                         MRA.WBDIS = 0, so a repeating consumer re-binds).
 * @return `k_ra8_ok`, or the first error encountered.
 * @retval k_ra8_err_null_ptr      @p cfg or @p ti was NULL (or a `cfg` pointer field).
 * @retval k_ra8_err_invalid_state @ref ra8_dtc_init has not run, so there is no
 *                                 vector table to write into.
 * @retval k_ra8_err_invalid_arg   @p icu_slot is outside the table, or @p cfg was rejected.
 * @retval k_ra8_err_not_found     @p icu_slot is not a registered ICU slot.
 * @pre @ref ra8_dtc_init succeeded with an @ref ra8_dtc_vector_table_t base.
 * @pre @p icu_slot was allocated by `ra8_isr_register()`.
 * @post On success the slot is armed and will activate the described transfer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_bind_activation(uint16_t                  icu_slot,
                                                const ra8_dtc_xfer_cfg_t* cfg,
                                                ra8_dtc_ti_t*             ti);

/**
 * @brief Put the DTC into MSTP-gated stop.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_enter_stop(void);

/**
 * @brief Exit MSTP-gated stop and re-arm the engine.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dtc_exit_stop(void);

#ifdef __cplusplus
}
#endif
