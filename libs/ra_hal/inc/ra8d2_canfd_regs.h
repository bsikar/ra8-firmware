/**
 * @file ra8d2_canfd_regs.h
 * @brief CANFD Lite controller register bases for the Renesas RA8D2
 *
 * @details
 * Two CANFD Lite instances on RA8D2:
 *  - CANFD0 at `0x40380000`
 *  - CANFD1 at `0x40382000`
 *
 * CANFD Lite is the controller used on most RA8 parts (vs. CANFD_B
 * on older RA6 devices). Full field layouts will be added when the
 * first CAN driver lands.
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
  k_ra_canfd0_base_addr = 0x40380000UL,
  k_ra_canfd1_base_addr = 0x40382000UL,
} ra_canfd_addr_t;

typedef enum : uint8_t {
  k_ra_canfd_instance_count = 2U,
} ra_canfd_limits_t;

#ifdef __cplusplus
}
#endif
