/**
 * @file ra8_ceu_internal.h
 * @brief Cross-TU surface for the ra8_ceu driver split.
 * @ingroup grp_hal_camera
 *
 * @details
 * Not part of the public API. Declares the init-time register
 * programming helpers that were promoted from TU-private statics so
 * the configuration packing/programming code can live in a separate
 * translation unit (``ra8_ceu_init_regs.c``) from the runtime-control
 * entry points (``ra8_ceu.c``). The two TUs share only the three
 * ``ra8_ceu_program_*`` helpers plus the stride derivation declared
 * below; everything else stays file-local. See CLAUDE.md "Test access to internal symbols (MC/DC
 * scope)".
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_attributes.h"
#include "ra8_ceu.h"

/**
 * @brief Program filter / format / camera-interface registers.
 *
 * @details
 * HUM Ch 60.2.10 (CFLCR), 60.2.7 (CAIFR), 60.2.2 (CAPCR), 60.2.3
 * (CAMCR). Pre-geometry phase of ``ra8_ceu_init``. Promoted from a
 * TU-private static so ``ra8_ceu_init`` (in ``ra8_ceu.c``) can call it
 * while the implementation lives in ``ra8_ceu_init_regs.c``.
 *
 * @param[in] cfg Validated config; must not be nullptr.
 *
 * @pre Module clock ungated, engine idle.
 * @pre Caller has validated ``cfg`` is non-nullptr.
 * @post Listed registers reflect ``cfg``.
 * @post No other module state is modified.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV void priv_ra8_ceu_program_format(const ra8_ceu_config_t* cfg);

/**
 * @brief Program the CMCYR/CAMOR/CAPWR geometry registers.
 *
 * @details
 * HUM Ch 60.2.4 (CMCYR p 3641), 60.2.5 (CAMOR p 3645) and 60.2.6
 * (CAPWR p 3646). Falls back to ``width_px`` / ``height_px`` when the
 * caller leaves ``x_capture_px`` / ``y_capture_lines`` at zero.
 * Promoted from a TU-private static so ``ra8_ceu_init`` (in
 * ``ra8_ceu.c``) can call it while the implementation lives in
 * ``ra8_ceu_init_regs.c``.
 *
 * @param[in] cfg Caller-supplied config; must not be nullptr.
 *
 * @pre Module clock ungated, engine idle.
 * @pre Caller has validated ``cfg`` is non-nullptr.
 * @post Geometry registers reflect ``cfg``.
 * @post No other module state is modified.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV void priv_ra8_ceu_program_geometry(const ra8_ceu_config_t* cfg);

/**
 * @brief Minimum destination stride in bytes implied by the descriptor.
 *
 * @details
 * HUM Ch 60.2.12 "CDWDR : Capture Destination Width Register" p 3654.
 * The bytes the engine writes per captured line are the scaled output
 * width (``scale.h_output_clip``, else ``x_capture_px``, else
 * ``width_px``) times ``bytes_per_pixel``. Data-enable fetch (JPEG and
 * other byte streams) has no pixel stride, so it answers zero. This is
 * the value ``ra8_ceu_init`` validates against and
 * ``priv_ra8_ceu_program_destination`` falls back to when the caller
 * leaves ``dst_stride`` at zero (#1362). Promoted from a TU-private
 * static so ``ra8_ceu_init`` (in ``ra8_ceu.c``) can reach it.
 *
 * @param[in] cfg Caller-supplied config; must not be nullptr.
 * @return Minimum stride in bytes; zero for data-enable fetch, and
 *         zero when the descriptor carries no width or no pixel size.
 *
 * @pre Caller has validated ``cfg`` is non-nullptr.
 * @post No register or module state is modified.
 *
 * @note Pure function; thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV uint32_t priv_ra8_ceu_min_stride_bytes(const ra8_ceu_config_t* cfg);

/**
 * @brief Program destination / firewall / output / event registers.
 *
 * @details
 * HUM Ch 60.2.11-60.2.22 pp 3651-3664. Post-geometry phase of
 * ``ra8_ceu_init``. Promoted from a TU-private static so
 * ``ra8_ceu_init`` (in ``ra8_ceu.c``) can call it while the
 * implementation lives in ``ra8_ceu_init_regs.c``.
 *
 * @param[in] cfg Validated config; must not be nullptr.
 *
 * @pre Module clock ungated, geometry registers programmed.
 * @pre Caller has validated ``cfg`` is non-nullptr.
 * @post Listed registers reflect ``cfg``.
 * @post No other module state is modified.
 *
 * @note Internal helper, not thread-safe.
 *
 * @since 0.1.0
 */
RA8_PRIV void priv_ra8_ceu_program_destination(const ra8_ceu_config_t* cfg);

#ifdef __cplusplus
}
#endif
