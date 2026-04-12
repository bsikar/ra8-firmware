/**
 * @file ra8d2_lpm_regs.h
 * @brief Low Power Mode (LPM) register layout for the Renesas RA8D2
 *
 * @details
 * The LPM / low-power control registers live inside the SYSC block
 * alongside CGC. This header exposes only the addresses and the
 * sleep-mode selector enum.
 *
 * Wake-up sources are extensive on the RA8D2 (WUPEN0/1 registers
 * cover 64 independent sources -- IRQs, AGTs, ULPTs, RTC alarms,
 * USB, LVD). Full field detail will be added as the first LPM
 * consumer lands.
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
  /* LPM registers sit inside the SYSC block starting at 0x4001E000;
   * offsets below are the ones documented in HUM section 11. */
  k_ra_lpm_sbycr_off   = 0x0C0U, /**< Standby Control.                 */
  k_ra_lpm_dpsbycr_off = 0x0C4U, /**< Deep Standby Control.            */
  k_ra_lpm_wupen0_off  = 0x0C8U, /**< Wakeup Interrupt Enable 0 (low). */
  k_ra_lpm_wupen1_off  = 0x0CCU, /**< Wakeup Interrupt Enable 1 (high).*/
} ra_lpm_off_t;

/**
 * @enum ra_sleep_mode_t
 * @brief Software-selectable sleep modes.
 */
typedef enum : uint8_t {
  k_ra_sleep_mode_sleep        = 0U, /**< CPU stopped, peripherals run.  */
  k_ra_sleep_mode_software_std = 1U, /**< Software standby.              */
  k_ra_sleep_mode_deep_sleep   = 2U, /**< Deep sleep (retain SRAM).      */
  k_ra_sleep_mode_deep_standby = 3U, /**< Deepest -- SRAM may be lost.   */
} ra_sleep_mode_t;

#ifdef __cplusplus
}
#endif
