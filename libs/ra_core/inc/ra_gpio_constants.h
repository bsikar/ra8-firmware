/**
 * @file ra_gpio_constants.h
 * @brief PSEL codes for peripheral pin routing on the RA8D2
 *
 * @details
 * Supplements `ra_port_constants.h` with peripheral-function codes
 * that go into the `PmnPFS.PSEL[28:24]` field when you want a pin
 * to act as a peripheral input/output instead of a plain GPIO.
 *
 * The codes come from RA8D2 Hardware User's Manual section 20.4
 * ("Peripheral I/O Table") -- only the common ones are listed
 * here; extend as drivers need more.
 *
 * Usage:
 *
 * @code{.c}
 * volatile uint32_t* pfs = ra_pfs_pmn(k_ra_port_3, k_ra_pin_0);
 * ra_pfs_pwpr_unlock();
 * *pfs = (uint32_t)k_ra_pfs_mask_pmr |
 *        ((uint32_t)k_ra_psel_sci0_txd << k_ra_pfs_bit_psel0);
 * ra_pfs_pwpr_lock();
 * @endcode
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra_psel_t
 * @brief Peripheral-function select codes (written to PSEL[4:0]).
 *
 * @details
 * The PSEL field is 5 bits wide. The list below is extracted from
 * HUM section 20.4. When a driver needs a code not listed here,
 * add it and cite the HUM section number in the doc comment.
 */
typedef enum : uint8_t {
  k_ra_psel_gpio       = 0x00U, /**< Plain GPIO mode (PMR=0).             */
  k_ra_psel_agt        = 0x01U, /**< AGT input/output.                    */
  k_ra_psel_gpt0       = 0x03U, /**< GPT timer I/O (and related).         */
  k_ra_psel_gpt1       = 0x04U,
  k_ra_psel_sci_async  = 0x04U, /**< SCI async serial (UART).             */
  k_ra_psel_sci_sync   = 0x05U, /**< SCI synchronous / simple SPI.        */
  k_ra_psel_sci_i2c    = 0x07U, /**< SCI simple I2C.                      */
  k_ra_psel_spi        = 0x06U, /**< RSPI / SPI.                          */
  k_ra_psel_iic        = 0x07U, /**< IIC controller-peripheral I2C.       */
  k_ra_psel_can        = 0x10U, /**< CANFD.                               */
  k_ra_psel_usb_fs     = 0x13U, /**< USB Full-Speed.                      */
  k_ra_psel_sdhi       = 0x12U, /**< SDHI SD / MMC.                       */
  k_ra_psel_ether_rmii = 0x11U, /**< Ethernet RMII / RGMII.               */
  k_ra_psel_qspi       = 0x14U, /**< QSPI / xSPI / Octo-SPI.              */
  k_ra_psel_glcdc      = 0x15U, /**< Graphics LCD Controller outputs.     */
  k_ra_psel_adc_b      = 0x00U, /**< Analog input (ASEL=1 + PMR=0).       */
  k_ra_psel_dac        = 0x00U, /**< DAC output (ASEL=1 + PMR=0).         */
} ra_psel_t;

#ifdef __cplusplus
}
#endif
