/**
 * @file ra8d2_rtc_regs.h
 * @brief Real-Time Clock (RTC) register layout for the Renesas RA8D2
 *
 * @details
 * RTC peripheral at `0x40202000`. Supports BCD calendar mode (year/
 * month/day/day-of-week/hours/minutes/seconds) and a simpler binary
 * counter mode. This header exposes the base address only; extend as
 * the first RTC consumer lands.
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
  k_ra_rtc_base_addr = 0x40202000UL,
} ra_rtc_addr_t;

#ifdef __cplusplus
}
#endif
