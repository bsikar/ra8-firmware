/**
 * @file ra8d2_adc_b_regs.h
 * @brief ADC_B (12/14-bit ADC) register layout for the Renesas RA8D2
 *
 * @details
 * The RA8D2 implements the "ADC_B" peripheral variant (not the older
 * "ADC" or "ADC_D"). Base address is `0x40338000`. Two ADC units live
 * on the same block, distinguished by channel masks. Driver code
 * should consult HUM section 34 ("12/14-bit A/D Converter ADC_B")
 * for the full field layout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

typedef enum : uintptr_t {
  k_ra_adc_b_base_addr = 0x40338000UL,
} ra_adc_b_addr_t;

typedef enum : uint8_t {
  k_ra_adc_b_unit_count = 2U,
} ra_adc_b_limits_t;

#ifdef __cplusplus
}
#endif
