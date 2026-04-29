/**
 * @file ra_dmac.h
 * @brief 8-channel DMA controller (DMAC0) driver
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Thin wrapper around the per-channel DMAC0 register window. Mirrors
 * the FSP `R_DMAC_*` transfer-API entry points using project-native
 * names:
 *
 *  - FSP `R_DMAC_Open`   -> `ra_dmac_start` (single-shot programme + enable)
 *  - FSP `R_DMAC_Close`  -> `ra_dmac_stop`
 *  - FSP `R_DMAC_Reset`  -> `ra_dmac_start` re-invocation
 *  - FSP `R_DMAC_Enable` -> implicit in `ra_dmac_start`
 *  - FSP `R_DMAC_SoftwareStart` -> not yet exposed; call when DCTG=00b
 *  - FSP `R_DMAC_InfoGet` -> not exposed (project pre-allocates buffers)
 *  - FSP `R_DMAC_Reload` / `R_DMAC_Reconfigure` -> not exposed yet
 *  - FSP `R_DMAC_CallbackSet` -> handled at `ra_dma` substrate layer
 *
 * Anything that needs the DTC's vector-table-driven semantics calls
 * `ra_dtc_*` directly; this driver only handles the channel-style
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

#include "ra_err.h"

/**
 * @enum ra_dmac_width_t
 * @brief Transfer element width.
 *
 * @details
 * Encodes the same 2-bit value as `DMTMD.SZ`. 64-bit transfers
 * (`DMTMD.SZ = 11b`) exist on the chip but are not yet plumbed
 * through this driver -- see HUM 17.2.10 p 738.
 */
typedef enum : uint8_t {
  k_ra_dmac_width_byte = 0U, /**< 8-bit transfers  (DMTMD.SZ = 00b). */
  k_ra_dmac_width_half = 1U, /**< 16-bit transfers (DMTMD.SZ = 01b). */
  k_ra_dmac_width_word = 2U, /**< 32-bit transfers (DMTMD.SZ = 10b). */
} ra_dmac_width_t;

/**
 * @enum ra_dmac_mode_t
 * @brief Transfer mode select. Mirrors FSP `transfer_mode_t` and HUM
 *        DMTMD.MD field (17.2.10 p 738).
 *
 * @details
 * Repeat / block / repeat-block require non-zero `block_count`
 * in `ra_dmac_config_t` (driven into `DMCRB`). Repeat-block also
 * requires `DMSRR / DMDRR / DMSBS / DMDBS` -- not yet exposed
 * here; callers needing it must extend the config.
 */
typedef enum : uint8_t {
  k_ra_dmac_mode_normal       = 0U, /**< DMTMD.MD = 00b. */
  k_ra_dmac_mode_repeat       = 1U, /**< DMTMD.MD = 01b. */
  k_ra_dmac_mode_block        = 2U, /**< DMTMD.MD = 10b. */
  k_ra_dmac_mode_repeat_block = 3U, /**< DMTMD.MD = 11b. */
} ra_dmac_mode_t;

/**
 * @enum ra_dmac_repeat_area_t
 * @brief Selects which side is the repeat / block area (DMTMD.DTS).
 *
 * @details
 * Only meaningful in repeat or block mode. In normal and repeat-
 * block mode the field is forced to "none" (HUM 17.2.10 p 738).
 */
typedef enum : uint8_t {
  k_ra_dmac_repeat_area_dest = 0U, /**< DTS = 00b. Dest is repeat area. */
  k_ra_dmac_repeat_area_src  = 1U, /**< DTS = 01b. Src is repeat area.  */
  k_ra_dmac_repeat_area_none = 2U, /**< DTS = 10b. No repeat area.      */
} ra_dmac_repeat_area_t;

/**
 * @struct ra_dmac_config_t
 * @brief DMAC channel configuration.
 *
 * @details
 * Mirrors the subset of FSP `transfer_info_t` that this project
 * currently programmes. Defaults (zero-initialised struct) yield a
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
/* cppcheck reads ra_dmac.h without seeing tests/ra_sim_dma.c or the
 * DMAC register accesses in libs/ra_hal/src/ra_dmac.c, so it flags
 * every field as unused even though the driver reads all of them. */
/* cppcheck-suppress-begin [unusedStructMember] */
typedef struct {
  uint32_t              src;         /**< Source address (DMSAR).         */
  uint32_t              dst;         /**< Destination address (DMDAR).    */
  uint16_t              count;       /**< Transfer count (DMCRA low).     */
  ra_dmac_width_t       width;       /**< Transfer element width.         */
  bool                  src_inc;     /**< true: SM = increment, else fixed. */
  bool                  dst_inc;     /**< true: DM = increment, else fixed. */
  ra_dmac_mode_t        mode;        /**< Transfer mode (DMTMD.MD).       */
  uint16_t              block_count; /**< DMCRB block/repeat count.       */
  ra_dmac_repeat_area_t repeat_area; /**< DMTMD.DTS area select.          */
  bool                  irq_each;    /**< Enable RPTIE/ESIE per repeat.   */
  bool                  enable_dtie; /**< Enable DMINT.DTIE on full end.  */
} ra_dmac_config_t;
/* cppcheck-suppress-end [unusedStructMember] */

/**
 * @brief Programme and enable a DMAC0 channel.
 *
 * @details
 * Mirrors FSP's `R_DMAC_Open` for the steady-state programme path.
 * Sequence (matches HUM 17.6 register-setting procedure):
 *  1. Bring DMAC0 out of MSTP (k_ra_mstp_dmac0_dtc0).
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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Channel armed.
 * @retval k_ra_err_null_ptr      `cfg` is NULL.
 * @retval k_ra_err_out_of_range  `channel` >= 8.
 * @retval k_ra_err_invalid_arg   `cfg->mode` or `cfg->width` invalid.
 *
 * @pre `ra_mstp_init()` has been called.
 * @pre IRQs masked or single-threaded init context.
 *
 * @post On success the channel is enabled and waiting for its
 *       configured trigger (software or ELC event via DELSR).
 *
 * @see ra_dmac_stop
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dmac_start(uint8_t channel, const ra_dmac_config_t* cfg);

/**
 * @brief Disable a DMAC0 channel and drop its MSTP reference.
 *
 * @param[in] channel DMAC0 channel index 0..7.
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Channel disabled.
 * @retval k_ra_err_out_of_range  `channel` >= 8.
 *
 * @pre `ra_dmac_start()` was called for `channel`.
 * @post DMCNT.DTE is 0 for `channel`.
 * @post One MSTP reference on `k_ra_mstp_dmac0_dtc0` is released.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dmac_stop(uint8_t channel);

#ifdef __cplusplus
}
#endif
