/**
 * @file ra_board_ek_ra8d2.c
 * @brief Board-support implementation for the EK-RA8D2 v1
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Translates board-coordinate APIs ("LED1", "Arduino D13") into HAL
 * calls. The BSP itself never touches MCU registers; ``ra_gpio_*`` /
 * ``ra_pfs_route_peripheral`` / ``ra_icu_configure_irq_pin`` /
 * ``ra_ssie_*`` carry every register write.
 *
 * Source-of-truth for the pin tables is
 * ``docs/reference/ek-ra8d2-v1-users-manual.pdf`` (R20UT5523EG0101
 * Rev 1.01, October 2025). Each pin entry's comment cites the table
 * and page in that document.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra_board_ek_ra8d2.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8d2_elc_regs.h"
#include "ra8d2_etha_regs.h"
#include "ra8d2_icu_regs.h"
#include "ra8d2_mstp_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_gpio_constants.h"
#include "ra_icu.h"
#include "ra_isr.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_rmac.h"
#include "ra_sci.h"
#include "ra_usb.h"

/* =============================================================================
 * Board identity strings (UM cover page, R20UT5523EG0101 Rev 1.01)
 * =============================================================================
 */

const char* const k_ra_board_name    = "EK-RA8D2 v1";
const char* const k_ra_board_doc_rev = "R20UT5523EG0101 Rev 1.01";
const char* const k_ra_board_mcu     = "R7KA8D2KFLCAC";

ra_err_t ra_board_get_info(ra_board_info_t* out)
{
  if (out == NULL) {
    return k_ra_err_invalid_arg;
  }
  out->name    = k_ra_board_name;
  out->doc_rev = k_ra_board_doc_rev;
  out->mcu     = k_ra_board_mcu;
  return k_ra_ok;
}

/* =============================================================================
 * 1. User LEDs (UM Table 24, page 31)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra_board_led_id_t`` to ``ra_port_pin_t``.
 *
 * @details
 * Index by ``ra_board_led_id_t`` (LED1=0, LED2=1, LED3=2). Entries
 * come straight from EK-RA8D2 UM Table 24 p 31.
 */
static const ra_port_pin_t s_led_pins[k_ra_board_led_count] = {
  [k_ra_board_led1] = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_0),  /**< P600 (blue).  */
  [k_ra_board_led2] = (ra_port_pin_t)RA_PIN(k_ra_port_3, k_ra_pin_3),  /**< P303 (green). */
  [k_ra_board_led3] = (ra_port_pin_t)RA_PIN(k_ra_port_10, k_ra_pin_7), /**< PA07 (red).   */
};

ra_err_t ra_board_led_pin(ra_board_led_id_t led, ra_port_pin_t* out_pin)
{
  if (out_pin == NULL) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)led >= (uint8_t)k_ra_board_led_count) {
    return k_ra_err_invalid_arg;
  }
  *out_pin = s_led_pins[led];
  return k_ra_ok;
}

ra_err_t ra_board_led_init(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_output_init(pin, k_ra_level_low);
}

ra_err_t ra_board_led_on(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_write(pin, k_ra_level_high);
}

ra_err_t ra_board_led_off(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_write(pin, k_ra_level_low);
}

ra_err_t ra_board_led_toggle(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_toggle(pin);
}

/* =============================================================================
 * 2. User switches (UM Table 25, page 32)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra_board_sw_id_t`` to ``ra_port_pin_t``.
 *
 * @details
 * SW1 -> P009, SW2 -> P008. Per UM Table 25 p 32.
 */
static const ra_port_pin_t s_sw_pins[k_ra_board_sw_count] = {
  [k_ra_board_sw1] = (ra_port_pin_t)RA_PIN(k_ra_port_0, k_ra_pin_9), /**< P009 / IRQ13-DS. */
  [k_ra_board_sw2] = (ra_port_pin_t)RA_PIN(k_ra_port_0, k_ra_pin_8), /**< P008 / IRQ12-DS. */
};

/** @brief Lookup table from ``ra_board_sw_id_t`` to ICU IRQ channel. */
static const uint8_t s_sw_irq_nums[k_ra_board_sw_count] = {
  [k_ra_board_sw1] = (uint8_t)k_ra_board_sw1_irq, /**< 13 (IRQ13-DS). */
  [k_ra_board_sw2] = (uint8_t)k_ra_board_sw2_irq, /**< 12 (IRQ12-DS). */
};

ra_err_t ra_board_sw_pin(ra_board_sw_id_t sw, ra_port_pin_t* out_pin)
{
  if (out_pin == NULL) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra_board_sw_count) {
    return k_ra_err_invalid_arg;
  }
  *out_pin = s_sw_pins[sw];
  return k_ra_ok;
}

ra_err_t ra_board_sw_init(ra_board_sw_id_t sw)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_sw_pin(sw, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_input_init(pin, k_ra_pull_up);
}

ra_err_t ra_board_sw_read(ra_board_sw_id_t sw, ra_board_sw_state_t* out_pressed)
{
  if (out_pressed == NULL) {
    return k_ra_err_invalid_arg;
  }
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_sw_pin(sw, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  ra_level_t lvl = k_ra_level_high;
  err            = ra_gpio_read(pin, &lvl);
  if (err != k_ra_ok) {
    return err;
  }
  /* Buttons are active-low: low level == pressed. */
  *out_pressed = (lvl == k_ra_level_low) ? k_ra_board_sw_pressed : k_ra_board_sw_released;
  return k_ra_ok;
}

ra_err_t ra_board_sw_attach_irq(ra_board_sw_id_t sw, ra_board_sw_irq_cb_t cb, void* ctx)
{
  if (cb == NULL) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra_board_sw_count) {
    return k_ra_err_invalid_arg;
  }
  /* Step 1: configure the ICU IRQ pin for falling-edge detection so a
   * button press (active-low; UM Table 25 p 32) latches IRQCR[12] /
   * IRQCR[13]. The digital filter samples at PCLKB to debounce
   * contact bounce; HUM Ch 14.2.12 (p 535) describes IRQCRi fields. */
  const ra_icu_irq_cfg_t cfg = {
    .sense      = k_ra_icu_irqmd_falling,
    .filter_div = k_ra_icu_fclksel_pclkb,
    .filter_en  = true,
  };
  ra_err_t err = ra_icu_configure_irq_pin(s_sw_irq_nums[sw], &cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 2: route the ELC event for IRQ12-DS / IRQ13-DS through an
   * IELSR slot and enable the matching NVIC line. SW1 -> IRQ13-DS
   * (event 0x00E), SW2 -> IRQ12-DS (event 0x00D). */
  const ra_elc_event_t evt = (sw == k_ra_board_sw1) ? k_ra_elc_event_icu_irq13
                                                    : k_ra_elc_event_icu_irq12;
  return ra_isr_register(evt, (ra_isr_handler_t)cb, ctx, k_ra_isr_prio_default, NULL);
}

/* =============================================================================
 * 3. Parallel-RGB GLCDC pin tables (UM Table 33 p 42)
 * =============================================================================
 */

/**
 * @brief RGB888-mode pin table for J1 (UM Table 33 p 42).
 *
 * @details
 * Common control + data lines routed when J1 is driven in 24-bit
 * RGB888 mode. Order matches the UM table (J1-1..J1-38).
 */
const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb888_pins[] = {
  {.signal = "BLEN",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)}, /**< J1-1 BLEN, P514. */
  {.signal = "SDA1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)}, /**< J1-2 SDA1, P511. */
  {.signal = "INT",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)}, /**< J1-3 INT,  P111. */
  {.signal = "SCL1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},              /**< J1-4 SCL1, P512. */
  {.signal = "RST", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)}, /**< J1-6 RST,  P606. */
  {.signal = "TCON0",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)}, /**< J1-9 VSYNC/TCON0, P806. */
  {.signal = "CLK",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)}, /**< J1-10 CLK,        P515. */
  {.signal = "TCON2",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)}, /**< J1-11 DE/TCON2,   P807. */
  {.signal = "TCON1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)}, /**< J1-12 HSYNC/TCON1,P805. */
  {.signal = "EXTCLK",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)}, /**< J1-13 EXTCLK,     P710. */
  {.signal = "TCON3",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)}, /**< J1-14 TCON3,      P513. */
  {.signal = "B1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)}, /**< J1-15 DATA1/B1,   P915. */
  {.signal = "B0",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)}, /**< J1-16 DATA0/B0,   P914. */
  {.signal = "B3",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)}, /**< J1-17 DATA3/B3,   P902. */
  {.signal = "B2",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)}, /**< J1-18 DATA2/B2,   P903. */
  {.signal = "B5",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)}, /**< J1-19 DATA5/B5,   P911. */
  {.signal = "B4",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)}, /**< J1-20 DATA4/B4,   P910. */
  {.signal = "B7",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)}, /**< J1-21 DATA7/B7,   P913. */
  {.signal = "B6",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)}, /**< J1-22 DATA6/B6,   P912. */
  {.signal = "G1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)}, /**< J1-23 DATA9/G1,   P207. */
  {.signal = "G0",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)}, /**< J1-24 DATA8/G0,   P904. */
  {.signal = "G3",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)}, /**< J1-25 DATA11/G3,  PB06. */
  {.signal = "G2",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)}, /**< J1-26 DATA10/G2,  PB07. */
  {.signal = "G5",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)}, /**< J1-27 DATA13/G5,  PB01. */
  {.signal = "G4",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)}, /**< J1-28 DATA12/G4,  PB05. */
  {.signal = "G7",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)}, /**< J1-29 DATA15/G7,  PB03. */
  {.signal = "G6",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)}, /**< J1-30 DATA14/G6,  PB04. */
  {.signal = "R1",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_0)}, /**< J1-31 DATA17/R1,  PB00. */
  {.signal = "R0",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_2)}, /**< J1-32 DATA16/R0,  PB02. */
  {.signal = "R3",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_11)}, /**< J1-33 DATA19/R3,  P711. */
  {.signal = "R2",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_7)}, /**< J1-34 DATA18/R2,  P707. */
  {.signal = "R5",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_13)}, /**< J1-35 DATA21/R5,  P713. */
  {.signal = "R4",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_12)}, /**< J1-36 DATA20/R4,  P712. */
  {.signal = "R7",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_15)}, /**< J1-37 DATA23/R7,  P715. */
  {.signal = "R6",
   .pin    = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_14)}, /**< J1-38 DATA22/R6,  P714. */
};

/**
 * @brief RGB666-mode pin table for J1 (UM Table 33 p 42).
 *
 * @details
 * In RGB666 the lowest two bits of each colour channel (R0, R1, G0,
 * G1, B0, B1) are not driven (board ties them off internally), so
 * the table omits the DATA0..DATA3 + DATA8..DATA9 + DATA16..DATA17
 * lines and drops DATA22..DATA23 (R6/R7) which are NC in RGB666.
 */
const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb666_pins[] = {
  {.signal = "BLEN", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)},
  {.signal = "SDA1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)},
  {.signal = "INT", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)},
  {.signal = "SCL1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},
  {.signal = "RST", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)},
  {.signal = "TCON0", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)},
  {.signal = "CLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)},
  {.signal = "TCON2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)},
  {.signal = "TCON1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)},
  {.signal = "EXTCLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)},
  {.signal = "TCON3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)},
  {.signal = "B3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)}, /**< J1-15 in RGB666. */
  {.signal = "B2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)},
  {.signal = "B5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)},
  {.signal = "B4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)},
  {.signal = "B7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)},
  {.signal = "B6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)},
  {.signal = "G3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)},
  {.signal = "G2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)},
  {.signal = "G5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)},
  {.signal = "G4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)},
  {.signal = "G7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)},
  {.signal = "G6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)},
  {.signal = "R3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)},
  {.signal = "R2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)},
  {.signal = "R5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)},
  {.signal = "R4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)},
  {.signal = "R7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_0)},
  {.signal = "R6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_2)},
};

/**
 * @brief RGB565-mode pin table for J1 (UM Table 33 p 42).
 *
 * @details
 * RGB565 uses 5+6+5 = 16 active data lines. R6/R7 + G6/G7 are NC,
 * and DATA31..DATA38 (R0..R7 lines) are NC. Same control pins as
 * RGB888.
 */
const ra_board_glcdc_pin_t g_ra_board_glcdc_rgb565_pins[] = {
  {.signal = "BLEN", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)},
  {.signal = "SDA1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)},
  {.signal = "INT", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)},
  {.signal = "SCL1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},
  {.signal = "RST", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)},
  {.signal = "TCON0", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)},
  {.signal = "CLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)},
  {.signal = "TCON2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)},
  {.signal = "TCON1", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)},
  {.signal = "EXTCLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)},
  {.signal = "TCON3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)},
  {.signal = "B4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)},
  {.signal = "B3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)},
  {.signal = "B6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)},
  {.signal = "B5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)},
  {.signal = "G2", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)},
  {.signal = "B7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)},
  {.signal = "G4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)},
  {.signal = "G3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)},
  {.signal = "G6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)},
  {.signal = "G5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)},
  {.signal = "R3", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)},
  {.signal = "G7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)},
  {.signal = "R5", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)},
  {.signal = "R4", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)},
  {.signal = "R7", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)},
  {.signal = "R6", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)},
};

const uint32_t g_ra_board_glcdc_rgb888_pin_count =
  sizeof(g_ra_board_glcdc_rgb888_pins) / sizeof(g_ra_board_glcdc_rgb888_pins[0]);
const uint32_t g_ra_board_glcdc_rgb666_pin_count =
  sizeof(g_ra_board_glcdc_rgb666_pins) / sizeof(g_ra_board_glcdc_rgb666_pins[0]);
const uint32_t g_ra_board_glcdc_rgb565_pin_count =
  sizeof(g_ra_board_glcdc_rgb565_pins) / sizeof(g_ra_board_glcdc_rgb565_pins[0]);

ra_err_t ra_board_glcdc_init(ra_board_glcdc_fmt_t fmt)
{
  const ra_board_glcdc_pin_t* table = NULL;
  uint32_t                    count = 0U;

  switch (fmt) {
    case k_ra_board_glcdc_fmt_rgb888:
      table = g_ra_board_glcdc_rgb888_pins;
      count = g_ra_board_glcdc_rgb888_pin_count;
      break;
    case k_ra_board_glcdc_fmt_rgb666:
      table = g_ra_board_glcdc_rgb666_pins;
      count = g_ra_board_glcdc_rgb666_pin_count;
      break;
    case k_ra_board_glcdc_fmt_rgb565:
      table = g_ra_board_glcdc_rgb565_pins;
      count = g_ra_board_glcdc_rgb565_pin_count;
      break;
    default:
      return k_ra_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    const ra_err_t err = ra_pfs_route_peripheral(table[i].pin, k_ra_psel_glcdc, "ra_board.glcdc");
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/* =============================================================================
 * 4. Audio CODEC (UM Table 32, page 38)
 * =============================================================================
 */

ra_err_t ra_board_audio_init(void)
{
  /* Routing only -- the SSIE controller bring-up is done by the
   * caller via ra_ssie_init(k_ra_board_audio_ssie_channel, &cfg)
   * once it has computed the right BCLK/WCLK divisors for the
   * desired sample rate. We just put the four DAI pins + I2C pair
   * into their alternate functions so SSIE/IIC can drive them.
   *
   * MCLK on PD06 stays in GPIO mode by default; the application
   * must route it to the right peripheral (SSIE EXTAL or CGC clock
   * out) explicitly because the EK-RA8D2 supports both.
   */
  const struct {
    ra_port_pin_t pin;
    ra_psel_t     psel;
    const char*   owner;
    /* The cast from ra_board_audio_pin_t to ra_port_pin_t is a re-tag
   * of the same underlying uint16_t (port << 8 | pin) value; the
   * static analyzer doesn't know the value space is identical. */
    /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  } routes[] = {
    {(ra_port_pin_t)k_ra_board_audio_pin_bclk, k_ra_psel_iic, "ra_board.audio.bclk"},
    {(ra_port_pin_t)k_ra_board_audio_pin_wclk, k_ra_psel_iic, "ra_board.audio.wclk"},
    {(ra_port_pin_t)k_ra_board_audio_pin_datin, k_ra_psel_iic, "ra_board.audio.datin"},
    {(ra_port_pin_t)k_ra_board_audio_pin_datout, k_ra_psel_iic, "ra_board.audio.datout"},
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_sda, k_ra_psel_iic, "ra_board.audio.i2c.sda"},
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_scl, k_ra_psel_iic, "ra_board.audio.i2c.scl"},
  };
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  /* TODO(bsp): k_ra_psel_iic is the IIC alt; SSIE uses a different
   * PSEL value not currently named in ra_gpio_constants.h. Once
   * k_ra_psel_ssie is added, swap the four DAI entries above. */
  for (uint32_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    const ra_err_t err = ra_pfs_route_peripheral(routes[i].pin, routes[i].psel, routes[i].owner);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

ra_err_t ra_board_audio_play_sample_block(const int16_t* buf, uint32_t len)
{
  if (buf == NULL) {
    return k_ra_err_invalid_arg;
  }
  if (len == 0U || (len % 2U) != 0U) {
    return k_ra_err_invalid_arg;
  }
  /* The HAL surface for the data path is complete:
   * ra_ssie_write_buffer(channel, const uint32_t* buffer,
   *                      uint16_t samples, uint16_t* out_written)
   * is exported from libs/ra_hal/inc/ra_ssie.h. The piece this BSP
   * veneer is missing is the one-time SSIE0 bring-up:
   *
   *   1. ra_board_audio_init() programmes the PFS routing but does
   *      NOT call ra_ssie_init(0, &cfg) -- the bclk/wclk divisors
   *      depend on the runtime sample rate the application picks,
   *      so the BSP cannot statically choose a ra_ssie_cfg_t.
   *   2. The DAI pins (P403..P406) are routed via k_ra_psel_iic in
   *      ra_board_audio_init() because ra_gpio_constants.h only
   *      defines k_ra_psel_iic = 0x07, not a dedicated
   *      k_ra_psel_ssie. The DA7212 controller-mode SSIE pins want
   *      a different PSEL value (chip HUM IO-Ports chapter,
   *      Table "Multiplexed Pin Function Selector").
   *
   * TODO(bsp): once (a) ra_gpio_constants.h gains k_ra_psel_ssie
   * with the right PSEL encoding, and (b) ra_board_audio_init() takes
   * a sample-rate argument so it can compute SSICR.CKDV and call
   * ra_ssie_init(0, &cfg), this function can be promoted to:
   *
   *   uint16_t written = 0U;
   *   return ra_ssie_write_buffer(k_ra_board_audio_ssie_channel,
   *                               (const uint32_t*)buf,
   *                               (uint16_t)(len / 2U),
   *                               &written);
   *
   * Until that lands, return k_ra_err_not_supported so the caller
   * does not block forever on an un-initialised SSIE FIFO. */
  return k_ra_err_not_supported;
}

/* =============================================================================
 * 5. Arduino header (UM Table 20, page 28)
 * =============================================================================
 */

ra_err_t ra_board_arduino_pin_init(ra_board_arduino_pin_t pin, ra_board_arduino_mode_t mode)
{
  switch (mode) {
    case k_ra_board_arduino_mode_input:
      return ra_gpio_input_init((ra_port_pin_t)pin, k_ra_pull_none);
    case k_ra_board_arduino_mode_input_pullup:
      return ra_gpio_input_init((ra_port_pin_t)pin, k_ra_pull_up);
    case k_ra_board_arduino_mode_output:
      return ra_gpio_output_init((ra_port_pin_t)pin, k_ra_level_low);
    default:
      return k_ra_err_invalid_arg;
  }
}

ra_err_t ra_board_arduino_gpio_write(ra_board_arduino_pin_t pin, ra_level_t level)
{
  return ra_gpio_write((ra_port_pin_t)pin, level);
}

ra_err_t ra_board_arduino_gpio_read(ra_board_arduino_pin_t pin, ra_level_t* out_level)
{
  if (out_level == NULL) {
    return k_ra_err_invalid_arg;
  }
  return ra_gpio_read((ra_port_pin_t)pin, out_level);
}

/* =============================================================================
 * 7. USB stubs (HS HAL not yet wired to BSP)
 * =============================================================================
 */

/**
 * @brief Shared CGC + MSTP bring-up for both USBHS modes.
 *
 * @details
 * Sequence per HUM Ch 9 (CGC, USBCKCR / USBCKDIVCR, p 365) and Ch 11
 * (MSTP, MSTPCRB12 USBHS, p 469):
 *   1. ``ra_cgc_usbhs_pll_enable()`` -- arms the PHY 12 MHz reference
 *      derived from the main XTAL.
 *   2. ``ra_mstp_init()`` -- ensures the MSTP refcount table exists
 *      (idempotent across BSP veneers).
 *   3. ``ra_mstp_enable(k_ra_mstp_usbhs)`` -- ungates MSTPCRB.MSTPB12
 *      so the controller's SYSCFG block is reachable.
 *
 * @return ra_err_t First non-OK result, or k_ra_ok on a clean run.
 *
 * @pre  ra_cgc_init() has been called.
 * @post On k_ra_ok, USBHS controller registers are clocked and ready
 *       for ra_usb_device_init / ra_usb_host_init.
 *
 * @note Not thread-safe.
 *
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_clock_and_mstp(void)
{
  ra_err_t err = ra_cgc_usbhs_pll_enable();
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_mstp_init();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_mstp_enable(k_ra_mstp_usbhs);
}

ra_err_t ra_board_usbhs_device_init(void)
{
  /* HUM Ch 9 (CGC) USBCKCR p 365 + HUM Ch 11 (MSTP) MSTPCRB12 p 469
   * stage the PHY clock and ungate the controller block; then the
   * generic device-mode entry in libs/ra_hal/inc/ra_usb.h drives
   * SYSCFG.SCKE / USBE / HSE and arms the device IRQ set. The
   * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 (J7 USBHS) does not itemise
   * an LDO-enable port pin -- the VBUS rail is gated by a discrete
   * USB-PD controller -- so no BSP-side PFS routing is needed. */
  ra_err_t err = internal_usbhs_clock_and_mstp();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_usb_device_init(k_ra_usb_speed_hs);
}

ra_err_t ra_board_usbhs_host_init(void)
{
  /* Symmetric to ra_board_usbhs_device_init. EK-RA8D2 v1 UM Rev 1.01
   * Table 28 lists USBHS_VBUSEN / USBHS_OVRCUR as PHY-controller
   * pins not routed to RA8D2 port pins, so VBUS sourcing is left to
   * the on-board USB-PD controller. ra_usb_host_init drives
   * SYSCFG.DCFM=1 / DRPD=1 / USBE=1 plus the DCP setup. */
  ra_err_t err = internal_usbhs_clock_and_mstp();
  if (err != k_ra_ok) {
    return err;
  }
  return ra_usb_host_init(k_ra_usb_speed_hs);
}

/* =============================================================================
 * 10. MIPI-DSI bring-up stub
 * =============================================================================
 */

ra_err_t ra_board_mipi_dsi_init(void)
{
  /* The DSI host bring-up function ra_mipi_dsi_init(const
   * ra_mipi_dsi_config_t* cfg) exists in libs/ra_hal/inc/ra_mipi_dsi.h
   * and is fully implemented (it programmes TXSETR / DSISETR /
   * timeouts / guard-band timing). What is missing here is:
   *
   *   1. A validated ra_mipi_dsi_config_t for the MIPI Graphics
   *      Expansion Board 1 panel (Renesas RTKMIPILCDB00000BE,
   *      Focus LCD E45RA-MW276-C, 854 x 480). The
   *      lane_count / clock_mode / timing / timeouts struct fields
   *      need values cross-checked against the panel datasheet --
   *      no committed reference exists under cmake/ yet.
   *   2. Up-front D-PHY bring-up. The HAL warns "The MIPI PHY
   *      (HUM Ch 64) must be brought up first" -- ra_mipi_phy.h
   *      exposes the PHY API but the EK-RA8D2-specific PLL
   *      configuration (REFDIV / FBDIV / lane bit-rate) is not yet
   *      committed.
   *   3. Panel-side init sequence: backlight enable on
   *      k_ra_board_mipi_dsi_backlight (P514), reset pulse on
   *      k_ra_board_mipi_dsi_reset_n (P606), and the per-panel DCS
   *      command stream that has to be replayed via
   *      ra_mipi_dsi_send_command() before the video link starts.
   *
   * TODO(bsp): once the panel descriptor lands in the BSP (e.g. as
   * a static const ra_mipi_dsi_config_t s_mipi_panel_cfg), promote
   * this function to:
   *
   *   ra_err_t err = ra_mipi_phy_init(&s_mipi_phy_cfg);
   *   if (err != k_ra_ok) { return err; }
   *   err = ra_mipi_dsi_init(&s_mipi_panel_cfg);
   *   if (err != k_ra_ok) { return err; }
   *   return ra_mipi_dsi_hs_clock_start();
   */
  return k_ra_err_not_supported;
}

/* =============================================================================
 * 11. J-Link OB VCOM serial bridge (UM Table 13, page 24)
 * =============================================================================
 */

/**
 * @brief Tracks whether ``ra_board_uart_console_init`` has succeeded.
 *
 * @details
 * Set to true after ``ra_sci_init`` returns ok and the PFS routes are
 * programmed. The write/read helpers refuse to forward to ra_sci when
 * this flag is false so callers cannot accidentally drive an
 * unconfigured channel.
 */
static bool s_uart_console_initialised = false;

/**
 * @brief Default PCLKB frequency assumed for SCI3 BRR calculation.
 *
 * @details
 * The RA8D2 reset-default CGC programming runs ICLK at 240 MHz and
 * PCLKB at 60 MHz (chip HUM Ch 12 CGC). The console init uses this
 * value when calling ``ra_sci_init``; applications that retune CGC
 * must call ``ra_sci_set_baud(3, baud, new_pclkb_hz)`` afterwards.
 */
typedef enum : uint32_t {
  k_ra_board_uart_console_default_pclkb_hz = 60000000UL,
} ra_board_uart_console_clock_t;

ra_err_t ra_board_uart_console_init(uint32_t baud)
{
  if (baud == 0U) {
    return k_ra_err_invalid_arg;
  }

  /* Route PD02 -> TXD3, PD03 -> RXD3 (UM Table 13 p 24). PSEL value
   * k_ra_psel_sci_async = 0x04 covers SCI async TXD/RXD per the chip
   * HUM Multiplexed Pin Function Selector. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra_err_t err = ra_pfs_route_peripheral((ra_port_pin_t)k_ra_board_uart_console_pin_txd,
                                         k_ra_psel_sci_async,
                                         "ra_board.uart.console.txd");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_pfs_route_peripheral((ra_port_pin_t)k_ra_board_uart_console_pin_rxd,
                                k_ra_psel_sci_async,
                                "ra_board.uart.console.rxd");
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  if (err != k_ra_ok) {
    return err;
  }

  const ra_sci_cfg_t cfg = {
    .baud      = baud,
    .data_bits = k_ra_sci_data_8,
    .parity    = k_ra_sci_parity_none,
    .stop_bits = k_ra_sci_stop_1,
    .pclk_hz   = (uint32_t)k_ra_board_uart_console_default_pclkb_hz,
  };
  err = ra_sci_init((uint8_t)k_ra_board_uart_console_sci_channel, &cfg);
  if (err != k_ra_ok) {
    return err;
  }
  s_uart_console_initialised = true;
  return k_ra_ok;
}

ra_err_t ra_board_uart_console_write(const uint8_t* data, size_t len)
{
  if (len == 0U) {
    return k_ra_ok;
  }
  if (data == NULL) {
    return k_ra_err_invalid_arg;
  }
  if (!s_uart_console_initialised) {
    return k_ra_err_not_initialized;
  }
  return ra_sci_write_polling((uint8_t)k_ra_board_uart_console_sci_channel, data, (uint32_t)len);
}

ra_err_t ra_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len)
{
  if (out_len == NULL) {
    return k_ra_err_invalid_arg;
  }
  *out_len = 0U;
  if (cap == 0U) {
    return k_ra_ok;
  }
  if (out == NULL) {
    return k_ra_err_invalid_arg;
  }
  if (!s_uart_console_initialised) {
    return k_ra_err_not_initialized;
  }

  /* Polled non-blocking drain: pull bytes while RDRF stays set, stop
   * the moment ra_sci_getc_polling reports nothing available. The cap
   * bounds the loop (NASA Rule 2). */
  for (size_t i = 0U; i < cap; ++i) {
    uint8_t        byte = 0U;
    const ra_err_t err =
      ra_sci_getc_polling((uint8_t)k_ra_board_uart_console_sci_channel, &byte);
    if (err != k_ra_ok) {
      /* No byte available yet -- treat as a non-blocking stop. */
      return k_ra_ok;
    }
    out[i] = byte;
    *out_len += 1U;
  }
  return k_ra_ok;
}

/* =============================================================================
 * 12. On-board RGMII Ethernet PHY (UM Table 26 + 27, page 33-34)
 * =============================================================================
 */

/**
 * @brief Pin table walked by ``ra_board_ethernet_init``.
 *
 * @details
 * Every entry is one of the sixteen Ethernet signals from UM Table 26
 * p 33. All sixteen need ``k_ra_psel_ether_rmii`` (the RA8D2 PSEL slot
 * 0x11 covers both RMII and RGMII -- the per-pin mux is the same; the
 * RMAC.MPIC.PIS field selects RGMII vs RMII for the data path).
 */
static const struct {
  ra_board_eth_pin_t pin;
  const char*        owner;
} s_eth_routes[] = {
  {k_ra_board_eth_pin_mdint, "ra_board.eth.mdint"},
  {k_ra_board_eth_pin_mdc, "ra_board.eth.mdc"},
  {k_ra_board_eth_pin_mdio, "ra_board.eth.mdio"},
  {k_ra_board_eth_pin_txd0, "ra_board.eth.txd0"},
  {k_ra_board_eth_pin_txd1, "ra_board.eth.txd1"},
  {k_ra_board_eth_pin_txd2, "ra_board.eth.txd2"},
  {k_ra_board_eth_pin_txd3, "ra_board.eth.txd3"},
  {k_ra_board_eth_pin_tx_ctl, "ra_board.eth.tx_ctl"},
  {k_ra_board_eth_pin_tx_clk, "ra_board.eth.tx_clk"},
  {k_ra_board_eth_pin_rxd0, "ra_board.eth.rxd0"},
  {k_ra_board_eth_pin_rxd1, "ra_board.eth.rxd1"},
  {k_ra_board_eth_pin_rxd2, "ra_board.eth.rxd2"},
  {k_ra_board_eth_pin_rxd3, "ra_board.eth.rxd3"},
  {k_ra_board_eth_pin_rx_ctl, "ra_board.eth.rx_ctl"},
  {k_ra_board_eth_pin_rx_clk, "ra_board.eth.rx_clk"},
  {k_ra_board_eth_pin_rstn, "ra_board.eth.rstn"},
};

ra_err_t ra_board_ethernet_init(void)
{
  /* Step 1: route every Ethernet pin to its ETHERC alternate. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  for (uint32_t i = 0U; i < sizeof(s_eth_routes) / sizeof(s_eth_routes[0]); ++i) {
    const ra_err_t err = ra_pfs_route_peripheral((ra_port_pin_t)s_eth_routes[i].pin,
                                                 k_ra_psel_ether_rmii,
                                                 s_eth_routes[i].owner);
    if (err != k_ra_ok) {
      return err;
    }
  }
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */

  /* Step 2: ETHA0 bring-up. RESET mode + no IRQs is enough for the
   * descriptor-ring init the application will do later. */
  const ra_etha_config_t etha_cfg = {
    .initial_mode = k_ra_etha_opc_reset,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  ra_err_t err = ra_etha_init((ra_etha_port_t)k_ra_board_eth_etha_port, &etha_cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 3: RMAC0 bring-up. RGMII / 1Gb / full-duplex matches the
   * board PHY's strap defaults; auto-negotiation will refine this
   * once the application calls ra_rmac_phy_auto_neg_start. */
  const ra_rmac_config_t rmac_cfg = {
    .rx_filter       = (ra_rmac_mrafc_t)(k_ra_rmac_mrafc_unicast_match
                                   | k_ra_rmac_mrafc_broadcast),
    .err_irq_enable  = 0U,
    .mon0_irq_enable = 0U,
    .mon1_irq_enable = 0U,
    .mon2_irq_enable = 0U,
    .phy_interface   = k_ra_rmac_pis_rgmii,
    .link_speed      = k_ra_rmac_lsc_1000mbit,
    .duplex          = k_ra_rmac_duplex_full,
  };
  err = ra_rmac_init((ra_rmac_port_t)k_ra_board_eth_rmac_port, &rmac_cfg);
  if (err != k_ra_ok) {
    return err;
  }
  return k_ra_ok;
}
