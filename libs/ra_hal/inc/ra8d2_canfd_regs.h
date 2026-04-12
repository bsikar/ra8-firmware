/**
 * @file ra8d2_canfd_regs.h
 * @brief CANFD Lite controller register layout for the Renesas RA8D2
 *
 * @details
 * Two CANFD Lite instances live at `0x40380000` (CANFD0) and
 * `0x40382000` (CANFD1). The RA8D2 uses the "CANFD Lite" IP (not the
 * older "CANFD_B" on some RA6 parts), which exposes:
 *
 *  - One **global block** shared between both controllers, with
 *    global configuration, nominal + data bit-rate prescalers,
 *    error counters, and a per-instance reset bit.
 *  - Two **channel blocks** -- one per controller -- with channel
 *    control / status, transmit queue, receive buffers, and
 *    acceptance filters.
 *  - A **message buffer** area for TX / RX frames.
 *
 * This header models the field layouts documented in RA8D2 HUM
 * section "CANFD Lite" -- enough to programme nominal + data
 * bit-rate, send one frame, and poll for RX. DMA hooks and the full
 * acceptance-filter bank come later.
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

/* =============================================================================
 * Per-channel register window
 * =============================================================================
 *
 * Offsets follow HUM Table "CANFD Lite register list". Only the
 * registers we currently touch are named; the rest of the channel
 * block is modelled as reserved padding so the offsets stay correct.
 */

/**
 * @struct r_canfd_channel_regs_t
 * @brief CANFD Lite per-channel register window (partial).
 */
typedef struct {
  volatile uint32_t CFDCNCFG;  /**< +0x000 Nominal Bit-Rate Config.        */
  volatile uint32_t CFDCNCTR;  /**< +0x004 Channel Control.                */
  volatile uint32_t CFDCNSTS;  /**< +0x008 Channel Status.                 */
  volatile uint32_t CFDCNERFL; /**< +0x00C Error Flag.                     */
  volatile uint8_t  _r0[0x10];
  volatile uint32_t CFDCNDCFG;  /**< +0x020 Data Bit-Rate Config (FD).      */
  volatile uint32_t CFDCNFDCFG; /**< +0x024 CAN FD Config.                  */
  volatile uint32_t CFDCNFDCTR; /**< +0x028 CAN FD Control.                 */
  volatile uint32_t CFDCNFDSTS; /**< +0x02C CAN FD Status.                  */
  volatile uint32_t CFDCNFDCRC; /**< +0x030 CAN FD CRC.                     */
} r_canfd_channel_regs_t;

/** @brief Get pointer to CANFD channel N (0..1). */
static inline volatile r_canfd_channel_regs_t* ra_canfd(uint8_t channel)
{
  if (channel >= (uint8_t)k_ra_canfd_instance_count) {
    return nullptr;
  }
  const uintptr_t base = (channel == 0U) ? k_ra_canfd0_base_addr : k_ra_canfd1_base_addr;
  return (volatile r_canfd_channel_regs_t*)base;
}

/* =============================================================================
 * Channel Control (CFDCNCTR) bit positions
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_cnctr_bit_chmdc = 0U,  /**< Channel Mode Control [2:0].          */
  k_ra_cnctr_bit_csli  = 4U,  /**< Channel Sleep Request.               */
  k_ra_cnctr_bit_bom   = 21U, /**< Bus-Off Mode.                        */
} ra_cnctr_bit_t;

/**
 * @enum ra_chmdc_mode_t
 * @brief Channel Mode Control (`CFDCNCTR.CHMDC[2:0]`) values.
 */
typedef enum : uint8_t {
  k_ra_chmdc_operation = 0U, /**< Bus operation mode.         */
  k_ra_chmdc_reset     = 1U, /**< Reset mode.                 */
  k_ra_chmdc_halt      = 2U, /**< Halt mode.                  */
} ra_chmdc_mode_t;

/* =============================================================================
 * Channel Status (CFDCNSTS) bit positions
 * =============================================================================
 */

typedef enum : uint8_t {
  k_ra_cnsts_bit_crstst = 0U, /**< Channel Reset Status.        */
  k_ra_cnsts_bit_chltst = 1U, /**< Channel Halt Status.         */
  k_ra_cnsts_bit_cslpst = 2U, /**< Channel Sleep Status.        */
  k_ra_cnsts_bit_epst   = 3U, /**< Error Passive Status.        */
  k_ra_cnsts_bit_bosst  = 4U, /**< Bus-Off Status.              */
} ra_cnsts_bit_t;

#ifdef __cplusplus
}
#endif
