/**
 * @file ra8d2_agt_regs.h
 * @brief Asynchronous General-Purpose Timer (AGT) register layout for RA8D2
 *
 * @details
 * The AGT is a 16-bit down-counter typically used as a tick timer or
 * a low-power wakeup source. RA8D2 has AGTX0..AGTX9 at `0x40221000`
 * with a `0x100` stride per instance. Each AGT can clock from PCLKB,
 * LOCO, subclock, or an underflow of another AGT.
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
  k_ra_agt0_base_addr = 0x40221000UL,
} ra_agt_addr_t;

typedef enum : uint16_t {
  k_ra_agt_channel_count  = 10U,
  k_ra_agt_channel_stride = 0x100U,
} ra_agt_limits_t;

typedef struct {
  volatile uint16_t AGT;       /**< +0x00 Counter.                */
  volatile uint16_t AGTCMA;    /**< +0x02 Compare Match A.        */
  volatile uint16_t AGTCMB;    /**< +0x04 Compare Match B.        */
  volatile uint16_t _r0;
  volatile uint8_t  AGTCR;     /**< +0x08 Control.                */
  volatile uint8_t  AGTMR1;    /**< +0x09 Mode 1.                 */
  volatile uint8_t  AGTMR2;    /**< +0x0A Mode 2.                 */
  volatile uint8_t  AGTIOC;    /**< +0x0B I/O Control.            */
  volatile uint8_t  AGTISR;    /**< +0x0C Event pin select.       */
  volatile uint8_t  AGTCMSR;   /**< +0x0D Compare select.         */
  volatile uint8_t  AGTIOSEL;  /**< +0x0E Pin select.             */
} r_agt_regs_t;

/** @brief Get pointer to AGT channel N. */
static inline volatile r_agt_regs_t* ra_agt(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_agt_channel_count) {
    return nullptr;
  }
  return (volatile r_agt_regs_t*)(k_ra_agt0_base_addr +
    ((uintptr_t)channel * (uintptr_t)k_ra_agt_channel_stride));
}

#ifdef __cplusplus
}
#endif
