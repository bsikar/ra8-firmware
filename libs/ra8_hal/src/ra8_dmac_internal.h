/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dmac_internal.h
 * @brief Test-access surface for ra8_dmac internal helpers (MC/DC).
 * @ingroup grp_hal_memory
 *
 * @details
 * Not part of the public API. Tests under tests/ MAY include this
 * header to drive compound boolean decisions that sit in TU-private
 * helpers behind the public ra8_dmac facade. See CLAUDE.md
 * "Test access to internal symbols (MC/DC scope)".
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_attributes.h"

/**
 * @brief Pure predicate for the DMTMD-DTS "no repeat" decision.
 *
 * @details
 * Returns true iff the transfer mode is NORMAL or REPEAT_BLOCK
 * (the two modes that disable DTS).  Promoted from the inline
 * compound OR at libs/ra8_hal/src/ra8_dmac.c inside
 * @c internal_dts_code so the decision can be driven directly under
 * @c -fcoverage-mcdc.  Both inputs are plain integers (the enum
 * values from @c ra8_dmac_mode_t); the helper performs no register
 * I/O and has no preconditions.
 *
 * @param[in] mode_normal_val        Numeric value of @c k_ra8_dmac_mode_normal.
 * @param[in] mode_repeat_block_val  Numeric value of @c k_ra8_dmac_mode_repeat_block.
 * @param[in] mode                   Candidate mode value.
 *
 * @return Boolean DTS-disabled predicate.
 * @retval true  Mode disables DTS (NORMAL or REPEAT_BLOCK).
 * @retval false Mode requires a non-zero DTS code.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition OR; N+1 = 3 vectors:
 *  - mode != normal && mode != repeat_block -> false
 *  - mode == normal                          -> true (varies left)
 *  - mode == repeat_block                    -> true (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_dmac_internal_mode_disables_dts(uint32_t mode_normal_val,
                                         uint32_t mode_repeat_block_val,
                                         uint32_t mode);

/**
 * @brief Pure predicate for the DMINT "extra IRQ bits" decision.
 *
 * @details
 * Returns true iff @p irq_each is set AND @p mode is not
 * REPEAT_BLOCK.  Promoted from the inline compound AND at
 * libs/ra8_hal/src/ra8_dmac.c inside @c internal_dmint_value.
 *
 * @param[in] irq_each               Boolean: per-block IRQ enable.
 * @param[in] mode_repeat_block_val  Numeric value of @c k_ra8_dmac_mode_repeat_block.
 * @param[in] mode                   Candidate mode value.
 *
 * @return Boolean enable-extra-IRQ predicate.
 * @retval true  Caller must OR in RPTIE | ESIE.
 * @retval false Caller leaves RPTIE / ESIE clear.
 *
 * @pre None.
 * @pre None.
 * @post No state mutated.
 * @post Return value depends solely on the three inputs.
 *
 * @note Test-access only. Pure function.
 *
 * @par MC/DC:
 * 2-condition AND; N+1 = 3 vectors:
 *  - irq_each=false, mode!=rb -> false
 *  - irq_each=true,  mode!=rb -> true  (varies left)
 *  - irq_each=true,  mode==rb -> false (varies right)
 *
 * @since 0.1.0
 */
RA8_PRIV
bool ra8_dmac_internal_dmint_extra_irq(bool     irq_each,
                                       uint32_t mode_repeat_block_val,
                                       uint32_t mode);

#ifdef __cplusplus
}
#endif
