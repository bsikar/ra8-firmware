/**
 * @file ra8d2_tsn_regs.h
 * @brief Temperature Sensor (TSN) register + calibration layout for the RA8D2
 *
 * @details
 * The TSN control block sits at `0x40235000`; calibration data is at
 * the flash-resident address `0x02C1EDA0`. Conversion: read the raw
 * TSN code via ADC_B, then subtract the calibration sample to get
 * degrees C.
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
  k_ra_tsn_ctrl_base_addr = 0x40235000UL,
  k_ra_tsn_cal_base_addr  = 0x02C1EDA0UL,
} ra_tsn_addr_t;

#ifdef __cplusplus
}
#endif
