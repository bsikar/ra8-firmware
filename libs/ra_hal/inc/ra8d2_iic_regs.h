/**
 * @file ra8d2_iic_regs.h
 * @brief IIC (I2C) register layout for the Renesas RA8D2
 *
 * @details
 * Three IIC channels (IIC0, IIC1, IIC2) at `0x4025E000` with stride
 * `0x100`. Each channel is a standard I2C controller/peripheral
 * block supporting standard (100 kHz) and fast (400 kHz) modes.
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
  k_ra_iic0_base_addr = 0x4025E000UL,
} ra_iic_addr_t;

typedef enum : uint16_t {
  k_ra_iic_channel_count  = 3U,
  k_ra_iic_channel_stride = 0x100U,
} ra_iic_limits_t;

typedef struct {
  volatile uint8_t ICCR1;     /**< +0x00 Control 1.       */
  volatile uint8_t ICCR2;     /**< +0x01 Control 2.       */
  volatile uint8_t ICMR1;     /**< +0x02 Mode 1.          */
  volatile uint8_t ICMR2;     /**< +0x03 Mode 2.          */
  volatile uint8_t ICMR3;     /**< +0x04 Mode 3.          */
  volatile uint8_t ICFER;     /**< +0x05 Function Enable. */
  volatile uint8_t ICSER;     /**< +0x06 Status Enable.   */
  volatile uint8_t ICIER;     /**< +0x07 Interrupt En.    */
  volatile uint8_t ICSR1;     /**< +0x08 Status 1.        */
  volatile uint8_t ICSR2;     /**< +0x09 Status 2.        */
  volatile uint8_t SARL0;     /**< +0x0A Slave Addr L 0.  */
  volatile uint8_t SARU0;     /**< +0x0B Slave Addr U 0.  */
  volatile uint8_t SARL1;     /**< +0x0C Slave Addr L 1.  */
  volatile uint8_t SARU1;     /**< +0x0D Slave Addr U 1.  */
  volatile uint8_t SARL2;     /**< +0x0E Slave Addr L 2.  */
  volatile uint8_t SARU2;     /**< +0x0F Slave Addr U 2.  */
  volatile uint8_t ICBRL;     /**< +0x10 Bit Rate Low.    */
  volatile uint8_t ICBRH;     /**< +0x11 Bit Rate High.   */
  volatile uint8_t ICDRT;     /**< +0x12 Transmit Data.   */
  volatile uint8_t ICDRR;     /**< +0x13 Receive Data.    */
} r_iic_regs_t;

/** @brief Get pointer to IIC channel N (0..2). */
static inline volatile r_iic_regs_t* ra_iic(uint8_t channel)
{
  if ((uint16_t)channel >= k_ra_iic_channel_count) {
    return nullptr;
  }
  return (volatile r_iic_regs_t*)(k_ra_iic0_base_addr +
    ((uintptr_t)channel * (uintptr_t)k_ra_iic_channel_stride));
}

#ifdef __cplusplus
}
#endif
