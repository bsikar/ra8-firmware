/**
 * @file ra8_dmac.h
 * @brief 8-channel DMA controller (DMAC0) driver
 * @ingroup grp_hal_memory
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Thin wrapper around the per-channel DMAC0 register window. Mirrors
 * the FSP `R_DMAC_*` transfer-API entry points using project-native
 * names:
 *
 *  - FSP `R_DMAC_Open`   -> `ra8_dmac_start` (single-shot programme + enable)
 *  - FSP `R_DMAC_Close`  -> `ra8_dmac_stop`
 *  - FSP `R_DMAC_Reset`  -> `ra8_dmac_start` re-invocation
 *  - FSP `R_DMAC_Enable` -> implicit in `ra8_dmac_start`
 *  - FSP `R_DMAC_SoftwareStart` -> not yet exposed; call when DCTG=00b
 *  - FSP `R_DMAC_InfoGet` -> not exposed (project pre-allocates buffers)
 *  - FSP `R_DMAC_Reload` / `R_DMAC_Reconfigure` -> not exposed yet
 *  - FSP `R_DMAC_CallbackSet` -> handled at `ra8_dma` substrate layer
 *
 * Anything that needs the DTC's vector-table-driven semantics calls
 * `ra8_dtc_*` directly; this driver only handles the channel-style
 * DMAC.
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

/**
 * @enum ra8_dmac_width_t
 * @brief Transfer element width.
 *
 * @details
 * Encodes the same 2-bit value as `DMTMD.SZ`. 64-bit transfers
 * (`DMTMD.SZ = 11b`) exist on the chip but are not yet plumbed
 * through this driver -- see HUM 17.2.10 p 738.
 */
typedef enum : uint8_t {
  k_ra8_dmac_width_byte = 0U, /**< 8-bit transfers  (DMTMD.SZ = 00b). */
  k_ra8_dmac_width_half = 1U, /**< 16-bit transfers (DMTMD.SZ = 01b). */
  k_ra8_dmac_width_word = 2U, /**< 32-bit transfers (DMTMD.SZ = 10b). */
} ra8_dmac_width_t;

/**
 * @enum ra8_dmac_mode_t
 * @brief Transfer mode select. Mirrors FSP `transfer_mode_t` and HUM
 *        DMTMD.MD field (17.2.10 p 738).
 *
 * @details
 * Repeat / block / repeat-block require non-zero `block_count`
 * in `ra8_dmac_config_t` (driven into `DMCRB`). Repeat-block also
 * requires `DMSRR / DMDRR / DMSBS / DMDBS` -- not yet exposed
 * here; callers needing it must extend the config.
 */
typedef enum : uint8_t {
  k_ra8_dmac_mode_normal       = 0U, /**< DMTMD.MD = 00b. */
  k_ra8_dmac_mode_repeat       = 1U, /**< DMTMD.MD = 01b. */
  k_ra8_dmac_mode_block        = 2U, /**< DMTMD.MD = 10b. */
  k_ra8_dmac_mode_repeat_block = 3U, /**< DMTMD.MD = 11b. */
} ra8_dmac_mode_t;

/**
 * @enum ra8_dmac_repeat_area_t
 * @brief Selects which side is the repeat / block area (DMTMD.DTS).
 *
 * @details
 * Only meaningful in repeat or block mode. In normal and repeat-
 * block mode the field is forced to "none" (HUM 17.2.10 p 738).
 */
typedef enum : uint8_t {
  k_ra8_dmac_repeat_area_dest = 0U, /**< DTS = 00b. Dest is repeat area. */
  k_ra8_dmac_repeat_area_src  = 1U, /**< DTS = 01b. Src is repeat area.  */
  k_ra8_dmac_repeat_area_none = 2U, /**< DTS = 10b. No repeat area.      */
} ra8_dmac_repeat_area_t;

/**
 * @struct ra8_dmac_config_t
 * @brief DMAC channel configuration.
 *
 * @details
 * Mirrors the subset of FSP `transfer_info_t` that this project
 * currently programmes. Defaults (zero-initialized struct) yield a
 * software-triggered, normal-mode transfer with both pointers fixed
 * and no IRQ -- safe for the existing CEU / SSIE / SCI / SPI / GPT
 * call sites.
 *
 * Fields:
 *  - `src` / `dst`        : 32-bit bus addresses (DMSAR / DMDAR).
 *  - `count`              : transfer count, units = `1 << width` bytes.
 *  - `width`              : transfer element size (DMTMD.SZ).
 *  - `src_inc` / `dst_inc`: legacy increment-only flags. When set, the
 *                           corresponding side is programmed with
 *                           `DMAMD.SM = 10b` / `DMAMD.DM = 10b`. When
 *                           clear, the side is fixed (`00b`).
 *  - `mode`               : DMTMD.MD transfer mode (defaults normal).
 *  - `block_count`        : DMCRB block / repeat count for non-normal
 *                           modes. Ignored in normal mode.
 *  - `repeat_area`        : DMTMD.DTS area select (only used in repeat
 *                           or block mode; otherwise forced to "none").
 *  - `irq_each`           : enable transfer-end IRQ even in
 *                           per-block / per-repeat-size modes
 *                           (DMINT.RPTIE | ESIE).
 *  - `enable_dtie`        : enable DMINT.DTIE (transfer-end IRQ at
 *                           full completion).
 */
/* cppcheck reads ra8_dmac.h without seeing tests/ra8_fake_dma.c or the
 * DMAC register accesses in libs/ra8_hal/src/ra8_dmac.c, so it flags
 * every field as unused even though the driver reads all of them. */
typedef struct {
  uint32_t               src;         /**< Source address (DMSAR).           */
  uint32_t               dst;         /**< Destination address (DMDAR).      */
  uint16_t               count;       /**< Transfer count (DMCRA low).       */
  ra8_dmac_width_t       width;       /**< Transfer element width.           */
  bool                   src_inc;     /**< true: SM = increment, else fixed. */
  bool                   dst_inc;     /**< true: DM = increment, else fixed. */
  ra8_dmac_mode_t        mode;        /**< Transfer mode (DMTMD.MD).         */
  uint16_t               block_count; /**< DMCRB block/repeat count.         */
  ra8_dmac_repeat_area_t repeat_area; /**< DMTMD.DTS area select.            */
  bool                   irq_each;    /**< Enable RPTIE/ESIE per repeat.     */
  bool                   enable_dtie; /**< Enable DMINT.DTIE on full end.    */
} ra8_dmac_config_t;

/**
 * @brief Programme and enable a DMAC0 channel.
 *
 * @details
 * Mirrors FSP's `R_DMAC_Open` for the steady-state programme path.
 * Sequence (matches HUM 17.6 register-setting procedure):
 *  1. Bring DMAC0 out of MSTP (k_ra8_mstp_dmac0_dtc0).
 *  2. Disable channel (DMCNT.DTE = 0).
 *  3. Programme DMTMD (mode, repeat area, size, software request).
 *  4. Programme DMAMD (source/dest update mode).
 *  5. Programme DMSAR / DMDAR / DMCRA / DMCRB / DMOFR.
 *  6. Programme DMINT (DTIE / RPTIE / ESIE per cfg).
 *  7. Set the global DMA module activation gate (DMAST.DMST = 1).
 *  8. Enable channel (DMCNT.DTE = 1).
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @param[in] cfg     Transfer descriptor.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Channel armed.
 * @retval k_ra8_err_null_ptr      `cfg` is NULL.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 * @retval k_ra8_err_invalid_arg   `cfg->mode` or `cfg->width` invalid.
 *
 * @pre `ra8_mstp_init()` has been called.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success the channel is enabled and waiting for its
 *       configured trigger (software or ELC event via DELSR).
 *
 * @see ra8_dmac_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dmac_start(uint8_t channel, const ra8_dmac_config_t* cfg);

/**
 * @brief Disable a DMAC0 channel and drop its MSTP reference.
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Channel disabled.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 *
 * @pre `ra8_dmac_start()` was called for `channel`.
 * @post DMCNT.DTE is 0 for `channel`.
 * @post One MSTP reference on `k_ra8_mstp_dmac0_dtc0` is released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dmac_stop(uint8_t channel);

/**
 * @enum ra8_dmac_addr_mode_t
 * @brief Per-side address-update mode (DMAMD.SM / DMAMD.DM).
 *
 * @details
 * Mirrors HUM 17.2.12 p 741. ``fixed`` keeps the DMSAR / DMDAR pointer
 * static across transfers; ``increment`` advances the pointer by the
 * transfer width; ``decrement`` walks backwards; ``offset`` adds the
 * value in DMOFR after each transfer.
 */
typedef enum : uint8_t {
  k_ra8_dmac_addr_fixed     = 0U, /**< 00b: address fixed.   */
  k_ra8_dmac_addr_offset    = 1U, /**< 01b: offset addition. */
  k_ra8_dmac_addr_increment = 2U, /**< 10b: increment.       */
  k_ra8_dmac_addr_decrement = 3U, /**< 11b: decrement.       */
} ra8_dmac_addr_mode_t;

/**
 * @typedef ra8_dmac_callback_fn_t
 * @brief Per-channel DMAC completion / half-complete callback signature.
 *
 * @details
 * Invoked from the matching DMAC ISR (or by the host test fake
 * via ``ra8_dmac_dispatch()``). ``ctx`` is the user-supplied opaque
 * pointer registered alongside the function.
 */
typedef void (*ra8_dmac_callback_fn_t)(void* ctx);

/**
 * @brief Programme a DMAC channel in repeat-area transfer mode.
 *
 * @details
 * Convenience wrapper that forces ``cfg->mode = k_ra8_dmac_mode_repeat``
 * before delegating to ``ra8_dmac_start()``. In repeat mode the
 * controller automatically rewinds either DMSAR or DMDAR (per
 * ``cfg->repeat_area``) every ``cfg->count`` transfers, so ring
 * buffers and double-buffered framebuffers can be expressed without
 * software intervention. HUM 17.2.10 p 738 (DMTMD.MD = 01b).
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @param[in] cfg     Transfer descriptor; ``mode`` is ignored.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Channel armed in repeat mode.
 * @retval k_ra8_err_null_ptr      `cfg` is NULL.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 * @retval k_ra8_err_invalid_arg   `cfg->width` invalid.
 *
 * @pre `ra8_mstp_init()` has been called.
 * @pre ``cfg->repeat_area`` selects either source or destination side.
 *
 * @post Channel is enabled and waiting for its trigger.
 * @post DMTMD.MD reads as 01b (repeat).
 *
 * @note Not thread-safe.
 * @see ra8_dmac_start
 * @see ra8_dmac_start_block
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dmac_start_repeat(uint8_t channel, const ra8_dmac_config_t* cfg);

/**
 * @brief Programme a DMAC channel in block-transfer mode.
 *
 * @details
 * Convenience wrapper that forces ``cfg->mode = k_ra8_dmac_mode_block``
 * before delegating to ``ra8_dmac_start()``. The controller transfers
 * ``cfg->count`` elements per trigger and then rewinds the repeat
 * side; ``cfg->block_count`` blocks are processed before the channel
 * is automatically disabled. HUM 17.2.10 p 738 (DMTMD.MD = 10b) and
 * HUM 17.2.9 (DMCRB block count).
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @param[in] cfg     Transfer descriptor; ``mode`` is ignored.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Channel armed in block mode.
 * @retval k_ra8_err_null_ptr      `cfg` is NULL.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 * @retval k_ra8_err_invalid_arg   `cfg->block_count` is zero.
 *
 * @pre `ra8_mstp_init()` has been called.
 * @pre ``cfg->block_count`` is non-zero.
 *
 * @post Channel is enabled and waiting for its trigger.
 * @post DMCRB carries ``cfg->block_count``.
 *
 * @note Not thread-safe.
 * @see ra8_dmac_start
 * @see ra8_dmac_start_repeat
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dmac_start_block(uint8_t channel, const ra8_dmac_config_t* cfg);

/**
 * @brief Override the DMAMD.SM / DMAMD.DM address-update modes.
 *
 * @details
 * Lets callers pick from the full 4-mode menu (fixed / offset /
 * increment / decrement) per HUM 17.2.12 p 741. The base
 * ``ra8_dmac_config_t`` only models fixed vs increment, so this entry
 * point is required when offset-addition or decrement is needed
 * (e.g. byte-reversed copy, ring-walk with stride).
 *
 * @param[in] channel   DMAC0 channel index 0..7.
 * @param[in] src_mode  New ``DMAMD.SM`` value.
 * @param[in] dest_mode New ``DMAMD.DM`` value.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                DMAMD updated.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 * @retval k_ra8_err_invalid_arg   Either mode > k_ra8_dmac_addr_decrement.
 *
 * @pre `ra8_dmac_start()` (or sibling) was called for `channel`.
 * @pre Channel is currently disabled (DMCNT.DTE = 0) before reprogramming.
 *
 * @post DMAMD.SM and DMAMD.DM reflect the requested codes.
 * @post All other DMAMD bits are preserved.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dmac_set_address_mode(uint8_t              channel,
                                                  ra8_dmac_addr_mode_t src_mode,
                                                  ra8_dmac_addr_mode_t dest_mode);

/**
 * @brief Attach a half-block IRQ callback to a DMAC channel.
 *
 * @details
 * Registers ``fn`` as the user-side handler invoked when DMINT.RPTIE
 * fires (repeat-size end -- HUM 17.2.11 p 739). The driver enables
 * RPTIE in the DMAC ISR plumbing so the half-complete callback fires
 * once per ``cfg->count`` transfers, independent of ``DTIE``.
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @param[in] fn      Callback function (NULL clears the slot).
 * @param[in] ctx     Opaque pointer passed to ``fn``.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Slot updated.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 *
 * @pre `ra8_dmac_start_*()` was called for `channel`.
 * @post ``ra8_dmac_dispatch_half(channel)`` invokes ``fn(ctx)``.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_dmac_attach_half_complete_handler(uint8_t channel, ra8_dmac_callback_fn_t fn, void* ctx);

/**
 * @brief Attach a transfer-end IRQ callback to a DMAC channel.
 *
 * @details
 * Registers a per-channel completion callback. The driver fires it
 * whenever ``ra8_dmac_dispatch(channel)`` is called by the matching
 * DMAC ISR (or, in host-test mode, by the test harness). Replaces any
 * earlier global callback that the substrate may have used.
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @param[in] fn      Callback function (NULL clears the slot).
 * @param[in] ctx     Opaque pointer passed to ``fn``.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Slot updated.
 * @retval k_ra8_err_out_of_range  `channel` >= 8.
 *
 * @pre `ra8_mstp_init()` has been called.
 * @post ``ra8_dmac_dispatch(channel)`` invokes ``fn(ctx)``.
 *
 * @note Not thread-safe; pair with IRQ masking.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_dmac_attach_callback(uint8_t channel, ra8_dmac_callback_fn_t fn, void* ctx);

/**
 * @brief Fire the per-channel completion callback (DMINT.DTIE path).
 *
 * @details
 * Test / ISR helper; consumes the slot installed by
 * ``ra8_dmac_attach_callback()``. Silently returns if no callback is
 * registered or the channel is out of range. Reads ``DMINT.DTIE``
 * (HUM Ch 16.2.7 "DMINT : DMA Interrupt Setting Register", p 612) to
 * decide whether the transfer-end IRQ is enabled before invoking the
 * callback.
 *
 * @param[in] channel DMAC0 channel index 0..7.
 *
 * @pre ``ra8_mstp_init()`` and ``ra8_dmac_start(channel, ...)`` have been called.
 * @pre Called from ISR context or unit-test driver.
 * @post Registered completion callback invoked at most once.
 * @post No state change if ``channel`` is out of range or no callback is set.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
void ra8_dmac_dispatch(uint8_t channel);

/**
 * @brief Fire the per-channel half-complete callback (DMINT.RPTIE path).
 *
 * @details
 * ISR / test helper for the repeat-block half-complete event. Consumes
 * the slot installed by ``ra8_dmac_attach_half_complete_handler()`` and
 * silently returns if no callback is registered or the channel is out
 * of range. Tied to ``DMINT.RPTIE`` (HUM Ch 16.2.7, p 612).
 *
 * @param[in] channel DMAC0 channel index 0..7.
 *
 * @pre ``ra8_mstp_init()`` and ``ra8_dmac_start_repeat(channel, ...)`` have been called.
 * @pre Called from ISR context or unit-test driver.
 * @post Registered half-complete callback invoked at most once.
 * @post No state change if ``channel`` is out of range or no callback is set.
 *
 * @note Thread safety: ISR context only; not re-entrant per channel.
 * @since 0.1.0
 */
void ra8_dmac_dispatch_half(uint8_t channel);

#ifdef __cplusplus
}
#endif
