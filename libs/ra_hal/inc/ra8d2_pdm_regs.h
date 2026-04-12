/**
 * @file ra8d2_pdm_regs.h
 * @brief PDM (Pulse Density Modulation) microphone interface base
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
  k_ra_pdm_base_addr = 0x40256000UL,
} ra_pdm_addr_t;

#ifdef __cplusplus
}
#endif
