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
 * ``ra8_ceu_program_*`` helpers declared below; everything else stays
 * file-local. See CLAUDE.md "Test access to internal symbols (MC/DC
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
RA8_PRIV void ra8_ceu_program_format(const ra8_ceu_config_t* cfg);

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
RA8_PRIV void ra8_ceu_program_geometry(const ra8_ceu_config_t* cfg);

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
RA8_PRIV void ra8_ceu_program_destination(const ra8_ceu_config_t* cfg);

#ifdef __cplusplus
}
#endif
