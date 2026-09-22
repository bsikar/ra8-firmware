/**
 * @file ra8_gpio_constants.h
 * @brief PSEL codes for peripheral pin routing on the RA8D2
 * @ingroup grp_core
 *
 * @details
 * Supplements `ra8_port_constants.h` with peripheral-function codes
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
 * volatile uint32_t* pfs = ra8_pfs_pmn(k_ra8_port_3, k_ra8_pin_0);
 * ra8_pfs_pwpr_unlock();
 * *pfs = (uint32_t)k_ra8_pfs_mask_pmr |
 *        ((uint32_t)k_ra8_psel_sci0_txd << k_ra8_pfs_bit_psel0);
 * ra8_pfs_pwpr_lock();
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
 * @enum ra8_psel_t
 * @brief Peripheral-function select codes (written to PSEL[4:0]).
 *
 * @details
 * The PSEL field is 5 bits wide. Values below are taken from
 * HUM Ch 20 "I/O Ports" section 20.6 "Peripheral Select Settings
 * for Each Product" (R01UH1065EJ0130, p 855-870). Each entry is
 * the binary code that appears in the per-port PSEL[4:0] tables
 * (Table 20.7..Table 20.21).
 *
 * When a driver needs a code not listed here, add it and cite the
 * HUM section number in the doc comment.
 */
typedef enum : uint8_t {
  k_ra8_psel_gpio        = 0x00U, /**< 00000b: GPIO / Hi-Z (PMR=0). HUM Tbl 20.7..20.21.   */
  k_ra8_psel_agt         = 0x01U, /**< 00001b: AGT input/output. HUM 20.6.                 */
  k_ra8_psel_gpt_trig    = 0x02U, /**< 00010b: GPT trigger pins (GTETRGA/B). HUM 20.6.     */
  k_ra8_psel_gpt0        = 0x03U, /**< 00011b: GPT timer I/O (GTIOCnA/B). HUM 20.6.        */
  k_ra8_psel_sci_async   = 0x04U, /**< 00100b: SCI async serial (UART). HUM 20.6.          */
  k_ra8_psel_sci_sync    = 0x05U, /**< 00101b: SCI synchronous / simple SPI. HUM 20.6.     */
  k_ra8_psel_spi         = 0x06U, /**< 00110b: RSPI / SPI. HUM 20.6.                       */
  k_ra8_psel_iic         = 0x07U, /**< 00111b: IIC / I3C controller-peripheral. HUM 20.6.  */
  k_ra8_psel_rtc_clkout  = 0x09U, /**< 01001b: RTC/CLKOUT/ACMPHS/ETHPHYCLK. HUM 20.6.      */
  k_ra8_psel_cac_adc     = 0x0AU, /**< 01010b: CAC / ADC16H trigger. HUM 20.6.             */
  k_ra8_psel_bus         = 0x0BU, /**< 01011b: External BUS interface. HUM 20.6.           */
  k_ra8_psel_sci_de      = 0x0DU, /**< 01101b: SCI driver-enable (DE). HUM 20.6.           */
  k_ra8_psel_sci_alt     = 0x0EU, /**< 01110b: SCI alternate function. HUM 20.6.           */
  k_ra8_psel_ceu         = 0x0FU, /**< 01111b: CEU camera capture. HUM 20.6.               */
  k_ra8_psel_can         = 0x10U, /**< 10000b: CANFD. HUM 20.6.                            */
  k_ra8_psel_ssie        = 0x12U, /**< 10010b: SSIE I2S audio. HUM 20.6.                   */
  k_ra8_psel_usb_fs      = 0x13U, /**< 10011b: USB Full-Speed. HUM 20.6.                   */
  k_ra8_psel_usb_hs      = 0x14U, /**< 10100b: USB High-Speed (e.g. USBHS_VBUS). HUM 20.6. */
  k_ra8_psel_sdhi        = 0x15U, /**< 10101b: SDHI SD / MMC. HUM 20.6.                    */
  k_ra8_psel_ether_mii   = 0x16U, /**< 10110b: ESWM (GMII / MII). HUM 20.6.                */
  k_ra8_psel_ether_rmii  = 0x17U, /**< 10111b: ESWM (RMII). HUM 20.6.                      */
  k_ra8_psel_ether_rgmii = 0x18U, /**< 11000b: ESWM (RGMII). HUM 20.6.                     */
  k_ra8_psel_glcdc       = 0x19U, /**< 11001b: Graphics LCD Controller outputs. HUM 20.6.  */
  k_ra8_psel_pdm         = 0x1BU, /**< 11011b: PDM-IF (PDMCLKn / PDMDATn). HUM 20.6.       */
  k_ra8_psel_qspi        = 0x1CU, /**< 11100b: OSPI / Octo-SPI / xSPI. HUM 20.6.           */
  k_ra8_psel_ulpt        = 0x1EU, /**< 11110b: ULPT ultra-low-power timer. HUM 20.6.       */
  k_ra8_psel_adc_b       = 0x00U, /**< Analog input (ASEL=1 + PMR=0).                      */
  k_ra8_psel_dac         = 0x00U, /**< DAC output (ASEL=1 + PMR=0).                        */
} ra8_psel_t;

#ifdef __cplusplus
}
#endif
