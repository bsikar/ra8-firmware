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
#include "ra8d2_pfs_regs.h"
#include "ra8d2_rmac_regs.h"
#include "ra_cgc.h"
#include "ra_err.h"
#include "ra_etha.h"
#include "ra_gpio_constants.h"
#include "ra_icu.h"
#include "ra_iic_b.h"
#include "ra_isr.h"
#include "ra_mipi_dsi.h"
#include "ra_mipi_phy.h"
#include "ra_mpc.h"
#include "ra_mstp.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"
#include "ra_rmac.h"
#include "ra_sci.h"
#include "ra_ssie.h"
#include "ra_time.h"
#include "ra_usb.h"

/* =============================================================================
 * Board identity strings (UM cover page, R20UT5523EG0101 Rev 1.01)
 * =============================================================================
 */

const char* const k_ra_board_name    = "EK-RA8D2 v1";
const char* const k_ra_board_doc_rev = "R20UT5523EG0101 Rev 1.01";
const char* const k_ra_board_mcu     = "R7KA8D2KFLCAC";

/* Ra board get info -- see implementation for details. */
ra_err_t ra_board_get_info(ra_board_info_t* out)
{
  if (out == nullptr) {
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

/* Ra board led pin -- see implementation for details. */
ra_err_t ra_board_led_pin(ra_board_led_id_t led, ra_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)led >= (uint8_t)k_ra_board_led_count) {
    return k_ra_err_invalid_arg;
  }
  *out_pin = s_led_pins[led];
  return k_ra_ok;
}

/* Ra board led init -- see implementation for details. */
ra_err_t ra_board_led_init(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_output_init(pin, k_ra_level_low);
}

/* Ra board led on -- see implementation for details. */
ra_err_t ra_board_led_on(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_write(pin, k_ra_level_high);
}

/* Ra board led off -- see implementation for details. */
ra_err_t ra_board_led_off(ra_board_led_id_t led)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_led_pin(led, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_write(pin, k_ra_level_low);
}

/* Ra board led toggle -- see implementation for details. */
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

/* Ra board sw pin -- see implementation for details. */
ra_err_t ra_board_sw_pin(ra_board_sw_id_t sw, ra_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra_board_sw_count) {
    return k_ra_err_invalid_arg;
  }
  *out_pin = s_sw_pins[sw];
  return k_ra_ok;
}

/* Ra board sw init -- see implementation for details. */
ra_err_t ra_board_sw_init(ra_board_sw_id_t sw)
{
  ra_port_pin_t pin = k_ra_pin_none;
  ra_err_t      err = ra_board_sw_pin(sw, &pin);
  if (err != k_ra_ok) {
    return err;
  }
  return ra_gpio_input_init(pin, k_ra_pull_up);
}

/* Ra board sw read -- see implementation for details. */
ra_err_t ra_board_sw_read(ra_board_sw_id_t sw, ra_board_sw_state_t* out_pressed)
{
  if (out_pressed == nullptr) {
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

/* Ra board sw attach irq -- see implementation for details. */
ra_err_t ra_board_sw_attach_irq(ra_board_sw_id_t sw, ra_board_sw_irq_cb_t cb, void* ctx)
{
  if (cb == nullptr) {
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
  const ra_elc_event_t evt =
    (sw == k_ra_board_sw1) ? k_ra_elc_event_icu_irq13 : k_ra_elc_event_icu_irq12;
  return ra_isr_register(evt, (ra_isr_handler_t)cb, ctx, k_ra_isr_prio_default, nullptr);
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

/**
 * @brief Test whether a J1 signal name corresponds to a GLCDC output.
 *
 * @details J1 connector pin table contains GLCDC outputs (TCONx, CLK,
 * R/G/B data lines) mixed with GPIO/I2C/clock-input control signals
 * (BLEN, RST, INT, SDA1, SCL1, EXTCLK).  Only the GLCDC outputs
 * should be routed via PSEL=glcdc and switched to output direction.
 *
 * @param[in] signal Human-readable signal name from the pin table.
 *
 * @return ``true`` for TCONx / CLK / R[0-9]* / G[0-9]* / B[0-9]*.
 * @retval true  Signal is a GLCDC peripheral output.
 * @retval false Signal is GPIO/I2C/clock-input.
 *
 * @pre ``signal`` is non-null and NUL-terminated.
 * @pre Signal names follow the EK-RA8D2 UM Table 33 conventions.
 * @post No side effects; pure inspection.
 * @post Return value is one of {true, false}.
 *
 * @note Single-threaded init-time helper.
 * @since 0.1.0
 */
/**
 * @brief Check whether a NUL-terminated string starts with `prefix`.
 *
 * @details Bounded to the longest prefix this BSP needs ("TCON",
 * 4 chars).  Implemented inline instead of `strncmp` to keep the BSP
 * free of `<string.h>` and to avoid compound boolean decisions that
 * would require MC/DC vectors.
 *
 * @param[in] s      NUL-terminated subject string.
 * @param[in] prefix NUL-terminated prefix (<= 4 chars).
 *
 * @return ``true`` iff every prefix character matches the matching
 *         position in `s`.
 * @retval true  Prefix matches.
 * @retval false At least one character differs.
 *
 * @pre `s` and `prefix` are both non-null and NUL-terminated.
 * @pre `prefix` is at most 4 characters long (compile-time invariant).
 * @post No side effects; pure inspection.
 * @post Return value is one of {true, false}.
 *
 * @note Init-time helper; single-threaded.
 * @since 0.1.0
 */
static bool ra_board_glcdc_signal_starts_with(const char* s, const char* prefix)
{
  for (uint32_t i = 0U; i < 4U; i++) { /* longest prefix here is "TCON" = 4. */
    if (prefix[i] == '\0') {
      return true;
    }
    if (s[i] != prefix[i]) {
      return false;
    }
  }
  return true;
}

/**
 * @brief Check whether a signal name is a GLCDC R/G/B data line.
 *
 * @details Matches the pattern `[RGB][0-9]` (e.g. "R0", "G7", "B3").
 * The digit check rules out "BLEN" (starts with B but second char is
 * 'L', not a digit).
 *
 * @param[in] signal NUL-terminated signal name from the pin table.
 *
 * @return ``true`` iff the first two characters form `[RGB][0-9]`.
 * @retval true  Signal is a colour-data line.
 * @retval false Signal is not a colour-data line.
 *
 * @pre `signal` is non-null and at least 2 characters (incl. NUL).
 * @pre Signal names follow EK-RA8D2 UM Table 33 conventions.
 * @post No side effects; pure inspection.
 * @post Return value is one of {true, false}.
 *
 * @note Init-time helper; single-threaded.
 * @since 0.1.0
 */
static bool ra_board_glcdc_signal_is_color_data(const char* signal)
{
  /* Match R0..R9 / G0..G9 / B0..B9; digit in [1] excludes BLEN. */
  const char c0 = signal[0];
  if (c0 != 'R') {
    if (c0 != 'G') {
      if (c0 != 'B') {
        return false;
      }
    }
  }
  const char c1 = signal[1];
  if (c1 < '0') {
    return false;
  }
  if (c1 > '9') {
    return false;
  }
  return true;
}

/**
 * @brief Test whether a J1 signal name corresponds to a GLCDC output.
 *
 * @details Returns true for TCONx / CLK / R[0-9]* / G[0-9]* / B[0-9]*;
 * false for BLEN / RST / SDA1 / SCL1 / INT / EXTCLK.  Used by
 * `ra_board_glcdc_init` to filter the J1 connector pin table down to
 * the pins that actually carry GLCDC peripheral signals.
 *
 * @param[in] signal Human-readable signal name from the pin table.
 *
 * @return ``true`` for GLCDC peripheral outputs.
 * @retval true  Signal is a GLCDC peripheral output.
 * @retval false Signal is GPIO/I2C/clock-input.
 *
 * @pre `signal` is non-null and NUL-terminated.
 * @pre Signal names follow EK-RA8D2 UM Table 33 conventions.
 * @post No side effects; pure inspection.
 * @post Return value is one of {true, false}.
 *
 * @note Single-threaded init-time helper.
 * @since 0.1.0
 */
static bool ra_board_glcdc_signal_is_output(const char* signal)
{
  if (ra_board_glcdc_signal_starts_with(signal, "TCON")) {
    return true;
  }
  if (ra_board_glcdc_signal_starts_with(signal, "CLK")) {
    return true;
  }
  return ra_board_glcdc_signal_is_color_data(signal);
}

/**
 * @brief Force a PFS register to PSEL=glcdc, PMR=1, PDR=1 in one write.
 *
 * @details ``ra_pfs_route_peripheral`` writes PSEL + PMR but leaves
 * PDR cleared, which keeps the pin in input direction.  GLCDC needs
 * its outputs in output direction; this helper does the combined
 * write directly under PWPR unlock so the chip drives the line.
 *
 * @param[in] pin J1 pin already validated to be a GLCDC output.
 *
 * @pre Caller has confirmed the pin is a GLCDC output via
 *      ``ra_board_glcdc_signal_is_output``.
 * @pre IOPORT module is powered (true at reset).
 * @post PFS = (psel_glcdc << 24) | PMR | PDR for the target pin.
 * @post PWPR is left in its locked state.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
static void ra_board_glcdc_force_pin_output(ra_port_pin_t pin)
{
  enum : uint32_t {
    k_pfs_psel_shift = 24U,
    k_pfs_pmr_bit    = 1U << 16,
    k_pfs_pdr_bit    = 1U << 2,
  };
  const ra_port_t          port = RA_PIN_PORT(pin);
  const ra_pin_t           bit  = RA_PIN_PIN(pin);
  volatile uint32_t* const pfs  = ra_pfs_pmn(port, bit);
  if (pfs == nullptr) {
    return;
  }
  ra_pfs_pwpr_unlock();
  *pfs = ((uint32_t)k_ra_psel_glcdc << k_pfs_psel_shift) | k_pfs_pmr_bit | k_pfs_pdr_bit;
  ra_pfs_pwpr_lock();
}

/* Ra board glcdc init -- see implementation for details. */
ra_err_t ra_board_glcdc_init(ra_board_glcdc_fmt_t fmt)
{
  const ra_board_glcdc_pin_t* table = nullptr;
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
    if (!ra_board_glcdc_signal_is_output(table[i].signal)) {
      continue; /* GPIO/I2C/clock-input -- not a GLCDC peripheral pin. */
    }
    const ra_err_t err = ra_pfs_route_peripheral(table[i].pin, k_ra_psel_glcdc, "ra_board.glcdc");
    if (err != k_ra_ok) {
      return err;
    }
    /* Override PFS with PSEL + PMR + PDR=1 so the chip drives the
     * pin as an output instead of leaving it as an input under
     * peripheral control. */
    ra_board_glcdc_force_pin_output(table[i].pin);
  }
  return k_ra_ok;
}

/**
 * @brief Implementation of `ra_board_lcd_panel_power_on`.
 *
 * @details See header for caller-facing contract.  Body drives
 * RESET_L low for 50 ms, releases it high for another 50 ms to let
 * the panel's internal POR finish, then asserts BLEN.
 *
 * @return Error code from the underlying GPIO operations.
 * @retval k_ra_ok               Panel out of reset, backlight on.
 * @retval k_ra_err_gpio_*       Underlying GPIO call failed.
 *
 * @pre IOPORT module powered (true at reset).
 * @pre `ra_time_init` has been called.
 * @post P606 RESET_L is high; P514 BLEN is high.
 * @post Returns within ~100 ms.
 *
 * @note Single-threaded init context; blocks on `ra_delay_ms`.
 * @since 0.1.0
 */
ra_err_t ra_board_lcd_panel_power_on(void)
{
  enum : uint32_t {
    k_reset_pulse_ms = 50U,
  };
  ra_err_t err = ra_gpio_output_init(k_ra_pin_lcd_reset_l, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_reset_pulse_ms);
  err = ra_gpio_write(k_ra_pin_lcd_reset_l, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms(k_reset_pulse_ms);
  return ra_gpio_output_init(k_ra_pin_lcd_blen, k_ra_level_high);
}

/* =============================================================================
 * 3b. Octo-SPI flash pin routing (UM Table 29 p 35, IS25LX512M-JHLE)
 * =============================================================================
 */

/**
 * @brief Lookup table for the 12 OCTA bus pins routed to PSEL=0x1C.
 *
 * @details
 * Order: CS, CK, DQS, DQ0..DQ7. The pin enums are uint16_t encodings
 * of (port << 8) | pin -- compatible with ``ra_port_pin_t`` value
 * space. The RESET_L pin is intentionally NOT in this table: it is
 * driven by the GPIO subsystem (active-low strap, not a peripheral
 * function). Source: EK-RA8D2 UM Table 29 "Octo-SPI Flash
 * Assignments" p 35.
 *
 * The ``ra_port_pin_t`` enum only enumerates the convenience
 * ``k_ra_pin_led*`` aliases; raw RA_PIN()-derived values are valid
 * data-space members but trigger the EnumCastOutOfRange checker, so
 * the cast cluster is wrapped in a NOLINT region matching the
 * convention used by ``internal_audio_route_pins`` below.
 */
/* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
static const ra_port_pin_t s_xspi_octa_pins[] = {
  (ra_port_pin_t)k_ra_board_xspi_cs,  /**< OSPI_FLASH_S_L,   P104. */
  (ra_port_pin_t)k_ra_board_xspi_clk, /**< OSPI_FLASH_C,     P808. */
  (ra_port_pin_t)k_ra_board_xspi_dqs, /**< OSPI_FLASH_DQS,   P801. */
  (ra_port_pin_t)k_ra_board_xspi_dq0, /**< OSPI_FLASH_DQ0,   P100. */
  (ra_port_pin_t)k_ra_board_xspi_dq1, /**< OSPI_FLASH_DQ1,   P803. */
  (ra_port_pin_t)k_ra_board_xspi_dq2, /**< OSPI_FLASH_DQ2,   P103. */
  (ra_port_pin_t)k_ra_board_xspi_dq3, /**< OSPI_FLASH_DQ3,   P101. */
  (ra_port_pin_t)k_ra_board_xspi_dq4, /**< OSPI_FLASH_DQ4,   P102. */
  (ra_port_pin_t)k_ra_board_xspi_dq5, /**< OSPI_FLASH_DQ5,   P800. */
  (ra_port_pin_t)k_ra_board_xspi_dq6, /**< OSPI_FLASH_DQ6,   P802. */
  (ra_port_pin_t)k_ra_board_xspi_dq7, /**< OSPI_FLASH_DQ7,   P804. */
};
/* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */

/**
 * @brief Hardware reset pulse + post-reset settle times for IS25LX512M-JHLE.
 *
 * @details
 * IS25LX512M datasheet specifies:
 *   - tRLRH (RESET_L low pulse width): min 100 ns (Ch 9.2 "Hardware Reset")
 *   - tRHSL (RESET_L high to first chip-select): min 100 us (Ch 9.2)
 *   - tPUW  (power-up window before first command):
 *           up to 10 ms (Ch 14 "Power-up / Power-down Timing")
 *
 * From cold boot we must honour tPUW, not just the much shorter
 * reset recovery. The previous 1 ms / 1 ms pulse was compliant with
 * tRLRH/tRHSL but not necessarily with tPUW: the controller could
 * clock RDID before the IS25LX internal regulator was stable, the
 * device would silently NAK, and CMDCMP would never assert -- LevelX
 * format then bailed at ``ra_xspi_flash_read_id``. Post-release
 * settle is now 15 ms, comfortably clearing tPUW from a true cold
 * boot. The pre-release low pulse stays at 1 ms (still 10000x the
 * 100 ns minimum tRLRH, just generous).
 */
typedef enum : uint32_t {
  k_ra_board_xspi_reset_low_ms  = 1U,  /**< Hold RESET_L low for >= tRLRH. */
  k_ra_board_xspi_reset_high_ms = 15U, /**< Wait >= tPUW before first CS.  */
} ra_board_xspi_reset_timing_t;

/* Ra board xspi pins init -- see implementation for details. */
ra_err_t ra_board_xspi_pins_init(void)
{
  /* Step 1: drive RESET_L low while the pin is still GPIO. PSEL=0x00
   * + PDR=output is the IS25LX512M strap that holds the chip in
   * reset (datasheet Ch 9.2). EK-RA8D2 UM Table 29 maps RESET_L to
   * P106. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_port_pin_t reset_pin = (ra_port_pin_t)k_ra_board_xspi_reset;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra_err_t err = ra_gpio_output_init(reset_pin, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_ra_board_xspi_reset_low_ms);

  /* Step 2: drive RESET_L high to release the chip. */
  err = ra_gpio_write(reset_pin, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  ra_delay_ms((uint32_t)k_ra_board_xspi_reset_high_ms);

  /* Step 3: route the 12 OCTA bus pins to PSEL=0x1C. PSEL 0x1C is
   * shared between the QSPI and Octo-SPI controllers per HUM Ch 20.6
   * "Multiplexed Pin Function Selector" p 871; the named constant
   * is ``k_ra_psel_qspi`` for legacy reasons. */
  const uint32_t count = (uint32_t)(sizeof(s_xspi_octa_pins) / sizeof(s_xspi_octa_pins[0]));
  for (uint32_t i = 0U; i < count; ++i) {
    err = ra_pfs_route_peripheral(s_xspi_octa_pins[i], k_ra_psel_qspi, "ra_board.xspi");
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

/**
 * @brief Tracks whether ``ra_board_audio_init`` has succeeded.
 */
static bool s_audio_initialized = false;

/**
 * @brief Supported PCM significant-bit-depth values for the DA7212 path.
 *
 * @details
 * Names every bit depth the BSP knows how to map to an SSIE DWL/SWL pair.
 * The DA7212 supports 16/24/32-bit native; 8/18/20/22 are accepted because
 * the SSIE itself supports those data-word widths and an application may
 * want to feed pre-padded streams.
 */
typedef enum : uint8_t {
  k_ra_audio_bits_8  = 8U,
  k_ra_audio_bits_16 = 16U,
  k_ra_audio_bits_18 = 18U,
  k_ra_audio_bits_20 = 20U,
  k_ra_audio_bits_22 = 22U,
  k_ra_audio_bits_24 = 24U,
  k_ra_audio_bits_32 = 32U,
} ra_audio_bit_depth_t;

/**
 * @brief Sample-frame channel counts the BSP accepts.
 */
typedef enum : uint8_t {
  k_ra_audio_channels_mono   = 1U,
  k_ra_audio_channels_stereo = 2U,
} ra_audio_channels_t;

/**
 * @brief Stereo-frame packing factor (two int16_t samples per 32-bit word).
 */
typedef enum : uint8_t {
  k_ra_audio_samples_per_word = 2U,
} ra_audio_pack_t;

/* Map ``bit_depth`` (significant bits) to SSIE DWL / SWL pair -- see implementation for details. */
static ra_err_t internal_audio_bits_to_word(uint8_t                bit_depth,
                                            ra_ssie_data_word_t*   out_dwl,
                                            ra_ssie_system_word_t* out_swl)
{
  switch ((ra_audio_bit_depth_t)bit_depth) {
    case k_ra_audio_bits_8:
      *out_dwl = k_ra_ssie_dwl_8;
      *out_swl = k_ra_ssie_swl_8;
      return k_ra_ok;
    case k_ra_audio_bits_16:
      *out_dwl = k_ra_ssie_dwl_16;
      *out_swl = k_ra_ssie_swl_16;
      return k_ra_ok;
    case k_ra_audio_bits_18:
      *out_dwl = k_ra_ssie_dwl_18;
      *out_swl = k_ra_ssie_swl_24;
      return k_ra_ok;
    case k_ra_audio_bits_20:
      *out_dwl = k_ra_ssie_dwl_20;
      *out_swl = k_ra_ssie_swl_24;
      return k_ra_ok;
    case k_ra_audio_bits_22:
      *out_dwl = k_ra_ssie_dwl_22;
      *out_swl = k_ra_ssie_swl_24;
      return k_ra_ok;
    case k_ra_audio_bits_24:
      *out_dwl = k_ra_ssie_dwl_24;
      *out_swl = k_ra_ssie_swl_24;
      return k_ra_ok;
    case k_ra_audio_bits_32:
      *out_dwl = k_ra_ssie_dwl_32;
      *out_swl = k_ra_ssie_swl_32;
      return k_ra_ok;
    default:
      return k_ra_err_invalid_arg;
  }
}

/* Route the six DA7212 audio pins (4 SSIE + 2 IIC) to their alt fns -- see implementation for details. */
static ra_err_t internal_audio_route_pins(void)
{
  /* k_ra_psel_ssie comes from HUM Ch 19 PFS PSEL field. MCLK on PD06
   * stays in GPIO mode -- the application picks SSIE EXTAL or CGC
   * clock-out explicitly. */
  const struct {
    ra_port_pin_t pin;
    ra_psel_t     psel;
    const char*   owner;
    /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  } routes[] = {
    {(ra_port_pin_t)k_ra_board_audio_pin_bclk, k_ra_psel_ssie, "ra_board.audio.bclk"},
    {(ra_port_pin_t)k_ra_board_audio_pin_wclk, k_ra_psel_ssie, "ra_board.audio.wclk"},
    {(ra_port_pin_t)k_ra_board_audio_pin_datin, k_ra_psel_ssie, "ra_board.audio.datin"},
    {(ra_port_pin_t)k_ra_board_audio_pin_datout, k_ra_psel_ssie, "ra_board.audio.datout"},
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_sda, k_ra_psel_iic, "ra_board.audio.i2c.sda"},
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_scl, k_ra_psel_iic, "ra_board.audio.i2c.scl"},
  };
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  for (uint32_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    const ra_err_t err = ra_pfs_route_peripheral(routes[i].pin, routes[i].psel, routes[i].owner);
    if (err != k_ra_ok) {
      return err;
    }
  }
  return k_ra_ok;
}

/**
 * @brief Build the SSIE0 controller-mode I2S config for the DA7212 link.
 *
 * @details
 * AUDIO_MCK / 4 lands close to 12.288 MHz / 4 = 3.072 MHz BCK for
 * 48 kHz x 2ch x 32-bit frames; finer rate matching is left to
 * applications that re-call ra_ssie_init() with a tuned divider.
 *
 * @param[in,out] channels See function signature.
 * @param[in,out] dwl See function signature.
 * @param[in,out] swl See function signature.
 * @return Result code or value; see implementation.
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @pre Caller has validated arguments.
 * @post Side effects bounded to documented state.
 * @post State reflects operation result.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_ssie_cfg_t
internal_audio_build_ssie_cfg(uint8_t channels, ra_ssie_data_word_t dwl, ra_ssie_system_word_t swl)
{
  return (ra_ssie_cfg_t){
    .role          = k_ra_ssie_role_master,
    .format        = (channels == (uint8_t)k_ra_audio_channels_mono) ? k_ra_ssie_format_monaural
                                                                     : k_ra_ssie_format_i2s,
    .data_word     = dwl,
    .system_word   = swl,
    .bclk_div      = k_ra_ssie_bclk_div_4,
    .use_gpt_clk   = false,
    .long_frame    = false,
    .bckp_rising   = false,
    .lrckp_low     = false,
    .spdp_high     = false,
    .byte_swap     = false,
    .lr_continue   = false,
    .bck_idle_stop = false,
    .enable_aucke  = true,
    .tx_threshold  = 0U,
    .rx_threshold  = 0U,
  };
}

/* Ra board audio init -- see implementation for details. */
ra_err_t ra_board_audio_init(uint32_t sample_rate_hz, uint8_t bit_depth, uint8_t channels)
{
  if (sample_rate_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  // mcdc-deactivated: ra_board_audio_init channel-validation guard; both conditions are exercised independently by tests/test_ra_board_audio_validation, but llvm-cov gates require an N+1 vector that holds one condition true while the other is false -- only the all-valid (mono/stereo) inputs are reachable in production wiring.
  if (channels != (uint8_t)k_ra_audio_channels_mono &&
      channels != (uint8_t)k_ra_audio_channels_stereo) {
    return k_ra_err_invalid_arg;
  }
  ra_ssie_data_word_t   dwl  = k_ra_ssie_dwl_16;
  ra_ssie_system_word_t swl  = k_ra_ssie_swl_16;
  ra_err_t              berr = internal_audio_bits_to_word(bit_depth, &dwl, &swl);
  if (berr != k_ra_ok) {
    return berr;
  }

  /* Step 1: route the four DAI pins to SSIE0 + the I2C control pair
   * to IIC1 (UM Table 32 p 38). */
  ra_err_t rerr = internal_audio_route_pins();
  if (rerr != k_ra_ok) {
    return rerr;
  }

  /* Step 2: bring up SSIE0 in I2S controller mode. */
  (void)sample_rate_hz;
  const ra_ssie_cfg_t cfg  = internal_audio_build_ssie_cfg(channels, dwl, swl);
  const ra_err_t      serr = ra_ssie_init((uint8_t)k_ra_board_audio_ssie_channel, &cfg);
  if (serr != k_ra_ok) {
    return k_ra_err_hw_init_failed;
  }
  s_audio_initialized = true;
  return k_ra_ok;
}

/* Ra board audio play sample block -- see implementation for details. */
ra_err_t ra_board_audio_play_sample_block(const int16_t* buf, uint32_t len)
{
  if (buf == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (len == 0U || (len % (uint32_t)k_ra_audio_samples_per_word) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!s_audio_initialized) {
    return k_ra_err_not_initialized;
  }
  /* Two int16_t samples (one stereo frame) pack into one 32-bit SSIE
   * FIFO word; len is guaranteed even by the check above. The buffer
   * is a contiguous PCM stream the SSIE FIFO consumes 32 bits at a
   * time. Reinterpret via uintptr_t so we neither (a) double-hop
   * through void* (bugprone-casting-through-void) nor (b) trip
   * cast-align by going int16_t* -> uint32_t* directly. The caller
   * contract requires ``buf`` to be 32-bit aligned (one stereo frame
   * naturally aligns); see ra_board_audio_play_sample_block docs. */
  const uint32_t        words   = len / (uint32_t)k_ra_audio_samples_per_word;
  const uintptr_t       addr    = (uintptr_t)buf;
  const uint32_t* const packed  = (const uint32_t*)addr; /* NOLINT(performance-no-int-to-ptr) */
  uint16_t              written = 0U;
  const ra_err_t        err =
    ra_ssie_write_buffer((uint8_t)k_ra_board_audio_ssie_channel, packed, (uint16_t)words, &written);
  if (err != k_ra_ok) {
    return err;
  }
  if (written != (uint16_t)words) {
    return k_ra_err_hw_timeout;
  }
  return k_ra_ok;
}

/* =============================================================================
 * 5. Arduino header (UM Table 20, page 28)
 * =============================================================================
 */

/* Ra board arduino pin init -- see implementation for details. */
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

/* Ra board arduino gpio write -- see implementation for details. */
ra_err_t ra_board_arduino_gpio_write(ra_board_arduino_pin_t pin, ra_level_t level)
{
  return ra_gpio_write((ra_port_pin_t)pin, level);
}

/* Ra board arduino gpio read -- see implementation for details. */
ra_err_t ra_board_arduino_gpio_read(ra_board_arduino_pin_t pin, ra_level_t* out_level)
{
  if (out_level == nullptr) {
    return k_ra_err_invalid_arg;
  }
  return ra_gpio_read((ra_port_pin_t)pin, out_level);
}

/* =============================================================================
 * 7. USB stubs (HS HAL not yet wired to BSP)
 * =============================================================================
 */

/**
 * @var s_usbhs_probe
 * @brief Temporary bisect probe for USBHS bring-up HardFault.
 *
 * @details
 * Stepped through the sub-calls of internal_usbhs_clock_and_mstp +
 * ra_usb_device_init so a JLink session can read which sub-step is in
 * flight when the worker thread takes the IACCVIOL/PRECISERR fault.
 * Values: 1 = pre cgc_usbhs_pll_enable, 2 = pre mstp_init, 3 = pre
 * mstp_enable(usbhs), 4 = pre ra_usb_device_init(hs), 5 = post
 * ra_usb_device_init returned. Remove once the offending sub-call is
 * identified.
 *
 * @note File-scope, single-writer (worker thread).
 * @since 0.1.0
 */
volatile uint32_t s_usbhs_probe = 0U;

/**
 * @enum usbhs_probe_step_t
 * @brief Bisect-probe step values for s_usbhs_probe.
 */
typedef enum : uint32_t {
  k_usbhs_probe_pre_pll_enable    = 1U, /**< before ra_cgc_usbhs_pll_enable. */
  k_usbhs_probe_pre_mstp_init     = 2U, /**< before ra_mstp_init.            */
  k_usbhs_probe_pre_mstp_enable   = 3U, /**< before ra_mstp_enable(usbhs).   */
  k_usbhs_probe_pre_usb_dev_init  = 4U, /**< before ra_usb_device_init(hs).  */
  k_usbhs_probe_post_usb_dev_init = 5U, /**< after ra_usb_device_init.       */
} usbhs_probe_step_t;

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
 *
 * @retval 0 Success or default value.
 * @pre Module has been initialized.
 * @post Side effects bounded to documented state.
 */
static ra_err_t internal_usbhs_clock_and_mstp(void)
{
  s_usbhs_probe = (uint32_t)k_usbhs_probe_pre_pll_enable;
  ra_err_t err  = ra_cgc_usbhs_pll_enable();
  if (err != k_ra_ok) {
    return err;
  }
  /* DO NOT call ra_mstp_init() here -- it gates ALL modules (IOPORT,
   * SCI, etc.) and the chip faults trying to access already-running
   * peripherals. ra_mstp_init must run exactly once per boot, before
   * any peripheral is brought up; the boot-time path handles that.
   * ra_mstp_enable below uses the existing ref-counted state. */
  s_usbhs_probe = (uint32_t)k_usbhs_probe_pre_mstp_enable;
  return ra_mstp_enable(k_ra_mstp_usbhs);
}

/* =============================================================================
 * U15 PI4IOE5V6408 I/O expander -- SW4 override (J7 USB role select etc.)
 *
 * EK-RA8D2 v1 UM Rev 1.01 Section 5.5.3 + Section 4 p 16:
 *   "The EK-RA8D2 features an I2C I/O Port Expander (PI4IOE5V6408) at
 *    U15 which has the I2C address 0x43. The port expander is connected
 *    to the configuration switches SW4 and allows the settings to be
 *    read (when the I/O expander port is set to input) or overridden
 *    (when the I/O expander port is set to output) by software."
 *
 * Register map and polarity convention come from Renesas's reference
 * driver in ``ra-fsp-examples/ek_ra8t2/board_cfg_switch.c`` (the EK-RA8T2
 * sister board uses an identical wiring).
 * =============================================================================
 */

/** @brief PI4IOE5V6408 7-bit I2C address on EK-RA8D2 v1 (SW4 sibling). */
typedef enum : uint8_t {
  k_ra_board_io_expander_addr = 0x43U,
} ra_board_io_expander_addr_t;

/**
 * @brief PI4IOE5V6408 register addresses.
 *
 * @details
 * The expander is auto-incrementing; we always do single-byte writes.
 * Values cite ``ra-fsp-examples/ek_ra8t2/board_cfg_switch.c``.
 */
typedef enum : uint8_t {
  k_ra_board_pi4ioe_reg_devid     = 0x01U, /**< Device ID register. */
  k_ra_board_pi4ioe_reg_iodir     = 0x03U, /**< 1 = output. */
  k_ra_board_pi4ioe_reg_output    = 0x05U, /**< 1 = HIGH (== SW4 OFF). */
  k_ra_board_pi4ioe_reg_hiz       = 0x07U, /**< 1 = output Hi-Z. */
  k_ra_board_pi4ioe_reg_pud_sel   = 0x0DU, /**< Pull-up/down select. */
  k_ra_board_pi4ioe_reg_input_lvl = 0x0FU, /**< Input state. */
} ra_board_pi4ioe_reg_t;

/**
 * @brief Magic-value writes for ``ra_board_io_expander_set_usbhs_device_mode``.
 *
 * @details
 * Polarity per FSP reference: ``OFF == output bit HIGH``. SW4-8 OFF
 * selects USB-HS device mode on J7, so bit 7 must be HIGH. We write
 * 0xFF (every SW4 channel in its mechanical-default OFF position),
 * which keeps every other SW4-gated peripheral at its EK-RA8D2 default
 * routing while still forcing SW4-8 = OFF.
 */
typedef enum : uint8_t {
  k_ra_board_pi4ioe_iodir_all_outputs = 0xFFU,
  k_ra_board_pi4ioe_output_all_high   = 0xFFU,
  k_ra_board_pi4ioe_hiz_none          = 0x00U,
} ra_board_pi4ioe_magic_t;

/** @brief I2C0 (IIC_B channel 0) configuration shared by every U15 access. */
typedef enum : uint32_t {
  k_ra_board_io_expander_bus_hz   = 100000U,    /**< 100 kHz Sm. */
  k_ra_board_io_expander_pclka_hz = 125000000U, /**< Reset CGC default. */
} ra_board_io_expander_clk_t;

/** @brief IIC_B channel index used for the U15 expander. */
typedef enum : uint8_t {
  k_ra_board_io_expander_iic_channel = 0U,
} ra_board_io_expander_channel_t;

/**
 * @brief Pins that carry SCL0 / SDA0 to U15 (chip HUM I/O Ports).
 *
 * @details
 * P400 = SCL0, P401 = SDA0 on the RA8D2 (HUM "Multiplexed Pin Function
 * Selector"). The EK-RA8D2 v1 schematic ties U15's SCL/SDA to this
 * same pair so that one IIC_B channel can drive both U15 and any
 * Pmod1-side I2C peripheral the user wires up.
 */
static const ra_port_pin_t k_ra_board_io_expander_pin_scl =
  (ra_port_pin_t)RA_PIN(k_ra_port_4, k_ra_pin_0); /**< SCL0 (P400). */
static const ra_port_pin_t k_ra_board_io_expander_pin_sda =
  (ra_port_pin_t)RA_PIN(k_ra_port_4, k_ra_pin_1); /**< SDA0 (P401). */

/* internal_io_expander_write_reg -- see implementation for details. */
static ra_err_t internal_io_expander_write_reg(uint8_t reg, uint8_t val)
{
  const uint8_t buf[2] = {reg, val};
  return ra_iic_b_write((uint8_t)k_ra_board_io_expander_iic_channel,
                        (uint8_t)k_ra_board_io_expander_addr,
                        buf,
                        sizeof(buf),
                        false);
}

/**
 * @var s_io_expander_probe
 * @brief Bisect-probe progress counter for ra_board_io_expander_set_usbhs_device_mode.
 *
 * @details
 * JLink-readable counter that records the last step the U15 bring-up
 * function reached before either succeeding or returning an error.
 * Mirrors the s_usbhs_probe pattern used by internal_usbhs_clock_and_mstp.
 *
 * Step values:
 *   1 = pre-PFS (entered function, before SCL/SDA route)
 *   2 = pre-init (PFS + open-drain done, before ra_iic_b_init)
 *   3 = pre-write-output (init done, before output-latch write)
 *   4 = pre-write-hiz (output-latch written, before Hi-Z clear)
 *   5 = pre-write-iodir (Hi-Z cleared, before iodir write)
 *   6 = success (all three U15 writes acknowledged)
 *
 * @note Volatile so the optimiser cannot lift the writes out of the
 *       step sequence; declared at file scope so JLink can resolve it
 *       by symbol name.
 * @since 0.1.0
 */
volatile uint32_t s_io_expander_probe = 0U;

/**
 * @var s_iic_b_mstp_enabled
 * @brief One-shot guard so the IIC0/I3C MSTP block is only ungated once.
 *
 * @details
 * ra_iic_b_init internally calls ra_mstp_enable(k_ra_mstp_i3c) on every
 * invocation, but the BSP also explicitly ungates IIC0 (MSTPB9) before
 * the very first call so the I3C/IIC_B controller has already been
 * powered when ra_iic_b_init reaches its register-init phase. The flag
 * keeps repeated calls (e.g. host vs device init) from over-counting
 * the MSTP refcount.
 *
 * @note File-scope, only mutated under single-threaded init context.
 * @since 0.1.0
 */
static bool s_iic_b_mstp_enabled = false;

/**
 * @brief Step values for s_io_expander_probe (see variable docs).
 */
typedef enum : uint32_t {
  k_io_exp_probe_pre_pfs       = 1U,
  k_io_exp_probe_pre_init      = 2U,
  k_io_exp_probe_pre_write_out = 3U,
  k_io_exp_probe_pre_write_hiz = 4U,
  k_io_exp_probe_pre_write_dir = 5U,
  k_io_exp_probe_success       = 6U,
} io_exp_probe_step_t;

/**
 * @brief Route P400/P401 to SCL0/SDA0 with N-channel open-drain enabled.
 * @details I2C requires NCODR on both signals; ra_pfs_route_peripheral
 *          leaves NCODR clear, so the route+open-drain pair runs per pin.
 * @return ra_err_t Error code from the first failing sub-call, else k_ra_ok.
 * @retval k_ra_ok All four register writes succeeded.
 * @pre IRQs masked or single-threaded init context.
 * @pre Caller has not already claimed P400/P401.
 * @post On success, P400/P401 are owned by the IIC_B mux in open-drain mode.
 * @post On failure, partially-claimed pins remain claimed (caller aborts boot).
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_io_expander_route_pins(void)
{
  ra_err_t err =
    ra_pfs_route_peripheral(k_ra_board_io_expander_pin_scl, k_ra_psel_iic, "ra_board.io_exp.scl0");
  if (err != k_ra_ok) {
    return err;
  }
  err = ra_mpc_set_open_drain(k_ra_port_4, k_ra_pin_0, true);
  if (err != k_ra_ok) {
    return err;
  }
  err =
    ra_pfs_route_peripheral(k_ra_board_io_expander_pin_sda, k_ra_psel_iic, "ra_board.io_exp.sda0");
  if (err != k_ra_ok) {
    return err;
  }
  return ra_mpc_set_open_drain(k_ra_port_4, k_ra_pin_1, true);
}

/**
 * @brief Issue the three U15 register writes that force USB-HS device mode.
 * @details Order matches the FSP reference (output-latch -> Hi-Z clear ->
 *          iodir) so the pin only flips to output once the latch is already
 *          driving the desired level. Updates s_io_expander_probe between
 *          writes so JLink can pinpoint a NACK.
 * @return ra_err_t Error code; k_ra_ok if all three writes ack.
 * @retval k_ra_ok U15 driving SW4-default pattern with SW4-8 OFF strap.
 * @pre ra_iic_b_init has already configured channel 0.
 * @pre I2C bus is idle.
 * @post On success, U15 IODIR=0xFF, OUTPUT=0xFF, HIZ=0x00.
 * @post On failure, s_io_expander_probe records the failing step.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_io_expander_program_u15(void)
{
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_out;
  ra_err_t err        = internal_io_expander_write_reg((uint8_t)k_ra_board_pi4ioe_reg_output,
                                                       (uint8_t)k_ra_board_pi4ioe_output_all_high);
  if (err != k_ra_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_hiz;
  err                 = internal_io_expander_write_reg((uint8_t)k_ra_board_pi4ioe_reg_hiz,
                                                       (uint8_t)k_ra_board_pi4ioe_hiz_none);
  if (err != k_ra_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_write_dir;
  return internal_io_expander_write_reg((uint8_t)k_ra_board_pi4ioe_reg_iodir,
                                        (uint8_t)k_ra_board_pi4ioe_iodir_all_outputs);
}

/* Ra board io expander set usbhs device mode -- see implementation for details. */
ra_err_t ra_board_io_expander_set_usbhs_device_mode(void)
{
  /* Step 1: route P400 -> SCL0 and P401 -> SDA0 with NCODR on both
   * (HUM Ch 20.6 PSEL table p 859). */
  s_io_expander_probe = (uint32_t)k_io_exp_probe_pre_pfs;
  ra_err_t err        = internal_io_expander_route_pins();
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 2: ungate IIC0 (MSTPB9, HUM Ch 11.2.7 MSTPCRB p 444) once
   * before ra_iic_b_init runs any controller register access.
   * Guarded by a one-shot flag so host + device init paths do not
   * double-increment the MSTP refcount. */
  if (!s_iic_b_mstp_enabled) {
    err = ra_mstp_enable(k_ra_mstp_iic0);
    if (err != k_ra_ok) {
      return err;
    }
    s_iic_b_mstp_enabled = true;
  }

  /* Step 3: bring IIC_B channel 0 up at 100 kHz. */
  s_io_expander_probe      = (uint32_t)k_io_exp_probe_pre_init;
  const ra_iic_b_cfg_t cfg = {
    .bus_hz   = (uint32_t)k_ra_board_io_expander_bus_hz,
    .pclka_hz = (uint32_t)k_ra_board_io_expander_pclka_hz,
  };
  err = ra_iic_b_init((uint8_t)k_ra_board_io_expander_iic_channel, &cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 4: program U15 in FSP-reference order. */
  err = internal_io_expander_program_u15();
  if (err != k_ra_ok) {
    return err;
  }
  s_io_expander_probe = (uint32_t)k_io_exp_probe_success;
  return k_ra_ok;
}

/**
 * @var s_usbhs_role_pin_probe
 * @brief Bisect-probe progress counter for the PD07 USB-HS role-select drive.
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34 ("USB High Speed"):
 *   "For a USB Device configuration, set PD07 to low and configure the RA
 *    MCU firmware to use the USB High Speed ports in device mode."
 * PD07 is a direct-drive MCU GPIO (PORT 13, pin 7) that selects the J7
 * role; the U15 I/O expander is an alternate override path but is not
 * required when PD07 is driven explicitly. JLink-readable so a bench
 * reflash can confirm the pin was reached and driven low.
 *
 * Step values:
 *   0 = not yet entered
 *   1 = pre ra_gpio_output_init(PD07, low)
 *   2 = output-init returned (success or failure recorded in s_usbhs_role_pin_err)
 *   3 = drive confirmed (gpio_init returned k_ra_ok)
 *
 * @since 0.1.0
 */
volatile uint32_t s_usbhs_role_pin_probe = 0U;

/**
 * @var s_usbhs_role_pin_err
 * @brief Last ra_err_t returned by the PD07 GPIO drive (0 = k_ra_ok).
 * @details JLink-readable so the bench operator can disambiguate a pin
 *          conflict from a port-mux failure.
 * @since 0.1.0
 */
volatile uint32_t s_usbhs_role_pin_err = 0U;

/** @brief Probe step values for s_usbhs_role_pin_probe. */
typedef enum : uint32_t {
  k_usbhs_role_probe_pre_init  = 1U,
  k_usbhs_role_probe_post_init = 2U,
  k_usbhs_role_probe_success   = 3U,
} usbhs_role_probe_step_t;

/**
 * @brief Drive PD07 low to strap the J7 USB-HS role line to "Device".
 *
 * @details
 * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34: PD07 is the J7 USB-HS role
 * select. Low = Device, High = Host. This is a plain MCU GPIO; the U15
 * I/O expander only matters if the firmware needs to override the SW4-8
 * strap from a different code path.
 *
 * @return ra_err_t Result of ra_gpio_output_init.
 * @retval k_ra_ok PD07 owned and driven low.
 * @pre IOPORT module clock is on (always-on after reset).
 * @pre Pin validator initialized.
 * @post On success, PD07 is GPIO-output low and owned by GPIO tag.
 * @post On any return, s_usbhs_role_pin_probe is updated and
 *       s_usbhs_role_pin_err records the gpio-init result.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_usbhs_role_select_device(void)
{
  s_usbhs_role_pin_probe = (uint32_t)k_usbhs_role_probe_pre_init;
  /* PD07 (port 13, pin 7). The ra_port_pin_t enum only pre-defines LED
   * pins, so any other packed value lands outside the enumerator set
   * and trips clang-analyzer EnumCastOutOfRange; suppress in the same
   * pattern used elsewhere in this file for board-specific pins. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_port_pin_t pd07 = (ra_port_pin_t)RA_PIN(k_ra_port_13, k_ra_pin_7);
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_err_t err     = ra_gpio_output_init(pd07, k_ra_level_low);
  s_usbhs_role_pin_err   = (uint32_t)err;
  s_usbhs_role_pin_probe = (uint32_t)k_usbhs_role_probe_post_init;
  if (err == k_ra_ok) {
    s_usbhs_role_pin_probe = (uint32_t)k_usbhs_role_probe_success;
  }
  return err;
}

/* Ra board usbhs device init -- see implementation for details. */
ra_err_t ra_board_usbhs_device_init(void)
{
  /* HUM Ch 9 (CGC) USBCKCR p 365 + HUM Ch 11 (MSTP) MSTPCRB12 p 469
   * stage the PHY clock and ungate the controller block; then the
   * generic device-mode entry in libs/ra_hal/inc/ra_usb.h drives
   * SYSCFG.SCKE / USBE / HSE and arms the device IRQ set.
   *
   * EK-RA8D2 v1 UM Rev 1.01 Section 6.2 p 34 ("USB High Speed"): J7's
   * role is selected by the MCU GPIO PD07 -- LOW = Device, HIGH = Host.
   * Drive PD07 low directly (no U15 I/O-expander dependency required;
   * U15 is only an SW4 override path, see Section 4.3.4 p 16 / Table 23
   * pullup-config p 31). The U15 helper is invoked best-effort below
   * for boards where PD07 alone is insufficient (e.g. SW4-8 mechanical
   * position contradicts the firmware intent), but its failure is
   * non-fatal because PD07 already strapped the role line. */
  const ra_err_t pd07_err = internal_usbhs_role_select_device();
  if (pd07_err != k_ra_ok) {
    return pd07_err;
  }

  /* Best-effort U15 override (logged via s_io_expander_probe). The U15
   * I2C path may NACK if SW4-5 is OFF (I2C/I3C-Select strap routes the
   * shared bus elsewhere) -- non-fatal because PD07 already strapped
   * the role. */
  const ra_err_t io_err = ra_board_io_expander_set_usbhs_device_mode();
  (void)io_err;
  ra_err_t err = internal_usbhs_clock_and_mstp();
  if (err != k_ra_ok) {
    return err;
  }
  s_usbhs_probe   = (uint32_t)k_usbhs_probe_pre_usb_dev_init;
  ra_err_t rc_dev = ra_usb_device_init(k_ra_usb_speed_hs);
  s_usbhs_probe   = (uint32_t)k_usbhs_probe_post_usb_dev_init;
  return rc_dev;
}

/* Ra board usbhs host init -- see implementation for details. */
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
 * 10. MIPI-DSI panel bring-up (J32 -- Renesas RTKMIPILCDB00000BE mezzanine)
 * =============================================================================
 */

/**
 * @brief Static placeholder geometry + line rate for the J32 panel.
 *
 * @details
 * The Renesas MIPI Graphics Expansion Board (RTKMIPILCDB00000BE) carries a
 * Focus-LCD E45RA-MW276-C 480 x 854 panel driven over a 2-lane D-PHY link.
 * The exact per-lane bit rate is panel-vendor information; the placeholder
 * 480 Mbps/lane lands in the HAL's PMUL=1/4 band so the PLL coefficient
 * block below is at least self-consistent at compile time.
 *
 * TODO(panel-datasheet): replace these three values with the row from the
 * RTKMIPILCDB00000BE / Focus E45RA-MW276-C datasheet.
 */
typedef enum : uint16_t {
  k_ra_board_mipi_panel_h_active       = 480U,
  k_ra_board_mipi_panel_v_active       = 854U,
  k_ra_board_mipi_panel_line_rate_mbps = 480U,
} ra_board_mipi_panel_geometry_t;

/**
 * @brief MIPI DSI host link-layer config for the J32 mezzanine.
 *
 * @details
 * Only fields whose values come from the SoC side (lane count, ECC / EoTP
 * defaults, ULPS wake-up) are filled in here. The guard-band timing block
 * and bus timeouts are left at the driver power-on defaults until the
 * panel datasheet pins down concrete numbers.
 *
 * TODO(panel-datasheet): populate ``timing`` (CLSTPTSETR / LPTRNSTSETR)
 * and ``timeouts`` (HSTXTOSETR, LRXHTOSETR, TATOSETR, PRESPTO*SETR) from
 * the panel datasheet -- the empty-init values below are accepted by the
 * driver but produce conservative blanking that may not meet the panel's
 * minimum HSA / HBP / HFP windows.
 */
static const ra_mipi_dsi_config_t s_mipi_panel_cfg = {
  .lane_count             = k_ra_mipi_dsi_lanes_2,
  .clock_mode             = k_ra_mipi_dsi_clock_non_continuous,
  .max_return_packet_size = 16U,
  .ulps_wakeup_period     = 0U,
  .ecc_check_enable       = true,
  .eotp_enable            = true,
  .scramble_enable        = false,
  .tearing_detect_enable  = true,
  .crc_check_vc_mask      = 0x01U, /* VC0 only -- the only VC J32 wires up. */
  .timing                 = {},    /* TODO(panel-datasheet). */
  .timeouts               = {},    /* TODO(panel-datasheet). */
};

/**
 * @brief Placeholder D-PHY HS/LP transition timing block.
 *
 * @details
 * The HAL exposes ``ra_mipi_phy_select_timing`` to look the right
 * DPHYTIM1..6 row up automatically; using it would be the right move once
 * the line rate is locked. The placeholder below carries a single non-zero
 * TINIT so the gap is obvious in a debugger.
 *
 * TODO(panel-datasheet): swap for a ``ra_mipi_phy_select_timing`` lookup
 * keyed on the confirmed panel line rate.
 */
static const ra_mipi_phy_timing_t s_mipi_phy_timing_placeholder = {
  .tinit = 1U,
};

/**
 * @brief MIPI D-PHY config for the J32 mezzanine.
 *
 * @details
 * PLL coefficients solve ``f = MOSC * (1/IDIV) * (NMUL+NFMUL) * (1/PMUL)``;
 * the placeholder values below assume MOSC=20 MHz and target 240 MHz PLL
 * out (480 Mbps/lane line rate, P=1/4 band).
 *
 * TODO(panel-datasheet): re-solve once the panel datasheet pins the line
 * rate down and the actual MOSC frequency on the EK-RA8D2 board is
 * confirmed; today's pclka_mhz=60 assumes the chip's CGC reset default.
 */
static const ra_mipi_phy_config_t s_mipi_phy_cfg = {
  .mode           = k_ra_mipi_phy_mode_dsi_master,
  .pclka_mhz      = 60U,
  .line_rate_mbps = (uint16_t)k_ra_board_mipi_panel_line_rate_mbps,
  .lane_count     = k_ra_mipi_phy_lane_count_2,
  .clk_mode       = k_ra_mipi_phy_clk_noncontinuous,
  .eotp           = k_ra_mipi_phy_eotp_enabled,
  .pll =
    {
      .idiv     = k_ra_mipi_phy_idiv_1,
      .pmul     = k_ra_mipi_phy_pmul_4,
      .nfmul    = k_ra_mipi_phy_nfmul_0_00,
      .nmul_int = 48U, /* TODO(panel-datasheet). */
    },
  .escdiv   = 0U,
  .p_timing = &s_mipi_phy_timing_placeholder,
};

/* Ra board mipi dsi init -- see implementation for details. */
ra_err_t ra_board_mipi_dsi_init(void)
{
  /* Step 1: PHY first -- HUM Ch 64.3.1 startup procedure. The HAL warns
   * "The MIPI PHY (HUM Ch 64) must be brought up first" before the DSI
   * host can clock its LP/HS lanes. */
  ra_err_t err = ra_mipi_phy_init(&s_mipi_phy_cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 2: DSI host link layer (HUM Ch 65). Programmes TXSETR / DSISETR
   * / guard-band timing / timeouts -- but does NOT start the HS clock
   * yet, so the application can splice in the panel-side reset pulse on
   * k_ra_board_mipi_dsi_reset_n (P606) and backlight enable on
   * k_ra_board_mipi_dsi_backlight (P514) between init and clock start. */
  err = ra_mipi_dsi_init(&s_mipi_panel_cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 3: kick the differential HS clock. After this returns the link
   * is HS-ready; callers can replay the panel DCS init stream via
   * ra_mipi_dsi_send_command() and finally call ra_mipi_dsi_video_start.
   *
   * TODO(panel-datasheet): the per-panel DCS command sequence (sleep-out,
   * pixel-format set, display-on, etc.) for the Focus E45RA-MW276-C is
   * not committed here -- the application currently owns it. */
  return ra_mipi_dsi_hs_clock_start();
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
static bool s_uart_console_initialized = false;

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

/* Ra board uart console init -- see implementation for details. */
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
  s_uart_console_initialized = true;
  return k_ra_ok;
}

/* Ra board uart console write -- see implementation for details. */
ra_err_t ra_board_uart_console_write(const uint8_t* data, size_t len)
{
  if (len == 0U) {
    return k_ra_ok;
  }
  if (data == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra_err_not_initialized;
  }
  return ra_sci_write_polling((uint8_t)k_ra_board_uart_console_sci_channel, data, (uint32_t)len);
}

/* Ra board uart console read -- see implementation for details. */
ra_err_t ra_board_uart_console_read(uint8_t* out, size_t cap, size_t* out_len)
{
  if (out_len == nullptr) {
    return k_ra_err_invalid_arg;
  }
  *out_len = 0U;
  if (cap == 0U) {
    return k_ra_ok;
  }
  if (out == nullptr) {
    return k_ra_err_invalid_arg;
  }
  if (!s_uart_console_initialized) {
    return k_ra_err_not_initialized;
  }

  /* Polled non-blocking drain: pull bytes while RDRF stays set, stop
   * the moment ra_sci_getc_polling reports nothing available. The cap
   * bounds the loop (NASA Rule 2). */
  for (size_t i = 0U; i < cap; ++i) {
    uint8_t        byte = 0U;
    const ra_err_t err  = ra_sci_getc_polling((uint8_t)k_ra_board_uart_console_sci_channel, &byte);
    if (err != k_ra_ok) {
      /* No byte available yet -- treat as a non-blocking stop. */
      return k_ra_ok;
    }
    out[i] = byte;
    *out_len += 1U;
  }
  return k_ra_ok;
}

/* Ra board uart console flush -- see implementation for details. */
ra_err_t ra_board_uart_console_flush(void)
{
  if (!s_uart_console_initialized) {
    return k_ra_err_not_initialized;
  }
  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- defer to
   * ra_sci_flush which spins on CSR.TEND for the SCI8 channel that
   * backs this console. Lets panic_halt() drain the failure log before
   * WFI gates the SCI clock. */
  return ra_sci_flush((uint8_t)k_ra_board_uart_console_sci_channel);
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

/**
 * @enum ra_board_eth_phy_reset_cycles_t
 * @brief Cycle-counted PHY reset timings (Cortex-M85 @ ~1 GHz).
 *
 * @details
 * The PEF7071 datasheet sec 6.3 "Reset" requires:
 *   - RST_N held LOW for >= 10 ms (we use >= 15 ms).
 *   - Post-release wait >= 50 ms before any MDIO access (we use >= 60 ms).
 *
 * `ra_delay_ms` is NOT usable here -- this function may run before
 * global IRQs are enabled (the typical app order is
 * setup_or_halt -> ra_isr_globals_enable), and ra_delay_ms is
 * WFI + SysTick, which hangs with IRQs masked. A `volatile nop`
 * loop is ~3 cycles/iter at 1 GHz so:
 *   - 5_000_000 iters >= 15 ms
 *   - 20_000_000 iters >= 60 ms
 */
typedef enum : uint32_t {
  k_ra_board_eth_phy_rst_low_iters  = 5000000UL,  /**< >= 15 ms LOW.  */
  k_ra_board_eth_phy_rst_post_iters = 20000000UL, /**< >= 60 ms HIGH. */
} ra_board_eth_phy_reset_cycles_t;

/**
 * @brief Hardware-reset the on-board PEF7071 PHY via GPIO.
 *
 * @details
 * Routes the board's PHY_RSTN net (P708) as a plain GPIO output,
 * asserts LOW for the required hold time, releases HIGH, and leaves
 * the pin in GPIO mode driven HIGH for the rest of the application.
 * The ETHERC alternate mux does NOT source RSTN, so the pin MUST
 * stay a GPIO to keep the PHY out of reset.
 *
 * @return Result code.
 * @retval k_ra_ok               PHY reset cycle completed.
 * @retval k_ra_err_invalid_arg  GPIO init / write rejected.
 *
 * @pre RA8D2 IOPORT clock is on (chip-default reset state is fine).
 * @pre Caller serialises (single-threaded init context).
 * @post P708 is in GPIO-output mode driven HIGH.
 * @post >= 60 ms has elapsed since RST_N rising edge.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_eth_phy_hw_reset(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_port_pin_t rstn_pin = (ra_port_pin_t)k_ra_board_eth_pin_rstn;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra_err_t err = ra_gpio_output_init(rstn_pin, k_ra_level_low);
  if (err != k_ra_ok) {
    return err;
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra_board_eth_phy_rst_low_iters; ++i) {
    __asm__ volatile("nop");
  }
  err = ra_gpio_write(rstn_pin, k_ra_level_high);
  if (err != k_ra_ok) {
    return err;
  }
  for (volatile uint32_t i = 0U; i < (uint32_t)k_ra_board_eth_phy_rst_post_iters; ++i) {
    __asm__ volatile("nop");
  }
  return k_ra_ok;
}

/**
 * @brief Route the 15 non-RSTN ETHERC pins to their ETHERC alternate.
 *
 * @details
 * Walks `s_eth_routes` and assigns every pin EXCEPT RSTN to
 * `k_ra_psel_ether_rmii`. RSTN is left in GPIO mode by
 * `internal_eth_phy_hw_reset`.
 *
 * @return Result code from `ra_pfs_route_peripheral`.
 * @retval k_ra_ok               All 15 alt-function pins routed.
 * @retval k_ra_err_invalid_arg  ra_pfs_route_peripheral rejected one pin.
 *
 * @pre internal_eth_phy_hw_reset has run.
 * @pre PFS write-protect is unlocked by ra_pfs_route_peripheral.
 * @post 15 pins (MDC/MDIO/TXDx/RXDx/TX_CTL/RX_CTL/TXCLK/RXCLK/MDINT)
 *       are routed to ETHERC alternate.
 * @post RSTN stays a GPIO (never touched here).
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
static ra_err_t internal_eth_route_alt_pins(void)
{
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_port_pin_t rstn_pin = (ra_port_pin_t)k_ra_board_eth_pin_rstn;
  for (uint32_t i = 0U; i < sizeof(s_eth_routes) / sizeof(s_eth_routes[0]); ++i) {
    if ((ra_port_pin_t)s_eth_routes[i].pin == rstn_pin) {
      continue;
    }
    const ra_err_t err = ra_pfs_route_peripheral((ra_port_pin_t)s_eth_routes[i].pin,
                                                 k_ra_psel_ether_rmii,
                                                 s_eth_routes[i].owner);
    if (err != k_ra_ok) {
      return err;
    }
  }
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  return k_ra_ok;
}

/* Ra board ethernet init -- see implementation for details. */
ra_err_t ra_board_ethernet_init(void)
{
  /* Step 0: hardware-reset the on-board PEF7071 PHY before anything
   * else touches MDIO. Without this the chip's first MDIO read
   * times out -- the PHY does not answer until RST_N has been
   * pulsed (UM Table 26/27 + PEF7071 datasheet sec 6.3 "Reset"). */
  ra_err_t err = internal_eth_phy_hw_reset();
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 1: route every remaining Ethernet pin to its ETHERC alternate
   * (RSTN intentionally stays a GPIO driven HIGH). */
  err = internal_eth_route_alt_pins();
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 2: ETHA0 bring-up. RESET mode + no IRQs is enough for the
   * descriptor-ring init the application will do later. */
  const ra_etha_config_t etha_cfg = {
    .initial_mode = k_ra_etha_opc_reset,
    .eaeie0_mask  = 0U,
    .eaeie1_mask  = 0U,
    .eaeie2_mask  = 0U,
  };
  err = ra_etha_init((ra_etha_port_t)k_ra_board_eth_etha_port, &etha_cfg);
  if (err != k_ra_ok) {
    return err;
  }

  /* Step 3: RMAC0 bring-up. RGMII / 1Gb / full-duplex matches the
   * board PHY's strap defaults; auto-negotiation will refine this
   * once the application calls ra_rmac_phy_auto_neg_start. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) */
  const ra_rmac_config_t rmac_cfg = {
    .rx_filter       = (ra_rmac_mrafc_t)(k_ra_rmac_mrafc_unicast_match | k_ra_rmac_mrafc_broadcast),
    .err_irq_enable  = 0U,
    .mon0_irq_enable = 0U,
    .mon1_irq_enable = 0U,
    .mon2_irq_enable = 0U,
    .phy_interface   = k_ra_rmac_pis_rgmii,
    .link_speed      = k_ra_rmac_lsc_1000mbit,
    .duplex          = k_ra_rmac_duplex_full,
  };
  err = ra_rmac_init((ra_rmac_port_t)k_ra_board_eth_rmac_port, &rmac_cfg);
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  if (err != k_ra_ok) {
    return err;
  }
  return k_ra_ok;
}
