/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file adc_internal.h
 * @brief Cross-TU constants shared by the ADC_B driver split.
 * @ingroup grp_hal_analog
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Not part of the public API. The RA8D2 ADC_B HAL driver is split across
 * two translation units:
 *
 * - ``adc.c`` -- converter bring-up, single-channel polling, scan-group
 *   orchestration, trigger / window-comparator / oversampling control;
 * - ``adc_selfdiag.c`` -- SIL3 / DO-178C self-diagnosis (modes 1/2/3)
 *   plus the internal temperature / reference-voltage extended-analog
 *   channel reads.
 *
 * Both translation units bounded-poll ``ADSR.ADACT0`` to wait for a
 * conversion to finish, so the busy-poll budget below is defined once
 * here. ``ra8_adc_const_t`` is the single source of truth for those
 * timing constants.
 *
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_adc_const_t
 * @brief Local timing / mask constants shared by the ADC_B driver TUs.
 */
typedef enum : uint32_t {
  k_ra8_adc_clk_wait_limit  = 100000UL,  /**< ADCLKSR settle-poll budget. */
  k_ra8_adc_busy_wait_limit = 2000000UL, /**< ADSR.ADACT busy-poll budget.
                                              2M spins on a 1 GHz CPU is
                                              ~2 ms wall clock -- a sane
                                              ADC conversion timeout on
                                              real silicon. Host tests
                                              leave ADACT0 idle (0) in the
                                              RAM-backed window, so the
                                              poll completes on its first
                                              read; nothing races this
                                              budget. */
} ra8_adc_const_t;

#ifdef __cplusplus
}
#endif
