/**
 * @file ra8d2_dmac_regs.h
 * @brief DMAC (Direct Memory Access Controller) register layout for the RA8D2
 *
 * @details
 * 8-channel DMAC at `0x4000A000` with `0x40` bytes per channel. Shares
 * a common trigger bank (`R_DMA_BASE` at `0x4000A800`). Each channel
 * supports single, block, and repeat transfer modes with flexible
 * trigger sources routed through the ELC.
 *
 * This header exposes a per-channel struct plus the channel count.
 * Full field layouts will be added when the first DMA consumer lands.
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
  k_ra_dmac0_base_addr = 0x4000A000UL,
  k_ra_dma_base_addr   = 0x4000A800UL, /**< Shared DMA controller regs. */
} ra_dmac_addr_t;

typedef enum : uint8_t {
  k_ra_dmac_channel_count  = 8U,
  k_ra_dmac_channel_stride = 0x40U,
  k_ra_dmac_tail_padding   = 0x0CU, /**< Bytes after DMBWR up to stride. */
} ra_dmac_limits_t;

/**
 * @struct r_dmac_channel_regs_t
 * @brief Per-channel DMAC register window.
 *
 * @details
 * Layout verified against FSP R_DMAC0_Type in R7KA8D2KF_core0.h
 * lines 5681-5871. Size = 52 (0x34) bytes per channel, with 12
 * bytes of tail padding up to the 0x40 channel stride.
 */
typedef struct {
  volatile uint32_t DMSAR; /**< +0x00 Source address.           */
  volatile uint32_t DMDAR; /**< +0x04 Destination address.      */
  volatile uint32_t DMCRA; /**< +0x08 Transfer count (32b).     */
  volatile uint32_t DMCRB; /**< +0x0C Block transfer count.     */
  volatile uint16_t DMTMD; /**< +0x10 Transfer Mode.            */
  volatile uint8_t  _r0;   /**< +0x12 reserved.                 */
  volatile uint8_t  DMINT; /**< +0x13 Interrupt Setting.        */
  volatile uint16_t DMAMD; /**< +0x14 Address Mode.             */
  volatile uint16_t _r1;   /**< +0x16 reserved.                 */
  volatile uint32_t DMOFR; /**< +0x18 Offset Register.          */
  volatile uint8_t  DMCNT; /**< +0x1C Transfer Enable.          */
  volatile uint8_t  DMREQ; /**< +0x1D Software Start.           */
  volatile uint8_t  DMSTS; /**< +0x1E Status.                   */
  volatile uint8_t  _r2;   /**< +0x1F reserved.                 */
  volatile uint32_t DMSRR; /**< +0x20 Source Reload Address.    */
  volatile uint32_t DMDRR; /**< +0x24 Dest Reload Address.      */
  volatile uint32_t DMSBS; /**< +0x28 Source Buffer Size.       */
  volatile uint32_t DMDBS; /**< +0x2C Dest Buffer Size.         */
  volatile uint8_t  DMBWR; /**< +0x30 Bufferable Write Enable.  */
  volatile uint8_t  _r3;   /**< +0x31 reserved.                 */
  volatile uint16_t _r4;   /**< +0x32 reserved.                 */
  volatile uint8_t  _r5[k_ra_dmac_tail_padding];
} r_dmac_channel_regs_t;

/** @brief Get pointer to DMAC channel N. */
static inline volatile r_dmac_channel_regs_t* ra_dmac(uint8_t channel)
{
  if (channel >= k_ra_dmac_channel_count) {
    return nullptr;
  }
  return (
    volatile r_dmac_channel_regs_t*)(k_ra_dmac0_base_addr +
                                     ((uintptr_t)channel * (uintptr_t)k_ra_dmac_channel_stride));
}

#ifdef __cplusplus
}
#endif
