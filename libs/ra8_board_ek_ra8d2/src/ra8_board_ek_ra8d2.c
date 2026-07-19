/**
 * @file ra8_board_ek_ra8d2.c
 * @brief Board-support implementation for the EK-RA8D2 v1
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Translates board-coordinate APIs ("LED1", "Arduino D13") into HAL
 * calls. The BSP itself never touches MCU registers; ``ra8_gpio_*`` /
 * ``ra8_pfs_route_peripheral`` / ``ra8_icu_configure_irq_pin`` /
 * ``ra8_ssie_*`` carry every register write.
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

#include "ra8_board_ek_ra8d2.h"

#include <stddef.h>
#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_elc_regs.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_icu.h"
#include "ra8_icu_regs.h"
#include "ra8_isr.h"
#include "ra8_pfs_regs.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"
#include "ra8_time.h"

/* =============================================================================
 * Board identity strings (UM cover page, R20UT5523EG0101 Rev 1.01)
 * =============================================================================
 */

const char* const k_ra8_board_name    = "EK-RA8D2 v1";
const char* const k_ra8_board_doc_rev = "R20UT5523EG0101 Rev 1.01";
const char* const k_ra8_board_mcu     = "R7KA8D2KFLCAC";

ra8_err_t ra8_board_get_info(ra8_board_info_t* out)
{
  if (out == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  out->name    = k_ra8_board_name;
  out->doc_rev = k_ra8_board_doc_rev;
  out->mcu     = k_ra8_board_mcu;
  return k_ra8_ok;
}

/* =============================================================================
 * 1. User LEDs (UM Table 24, page 31)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra8_board_led_id_t`` to ``ra8_port_pin_t``.
 *
 * @details
 * Index by ``ra8_board_led_id_t`` (LED1=0, LED2=1, LED3=2). Entries
 * come straight from EK-RA8D2 UM Table 24 p 31.
 */
static const ra8_port_pin_t s_led_pins[k_ra8_board_led_count] = {
  [k_ra8_board_led1] = (ra8_port_pin_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_0),  /**< P600 (blue).  */
  [k_ra8_board_led2] = (ra8_port_pin_t)RA8_PIN(k_ra8_port_3, k_ra8_pin_3),  /**< P303 (green). */
  [k_ra8_board_led3] = (ra8_port_pin_t)RA8_PIN(k_ra8_port_10, k_ra8_pin_7), /**< PA07 (red).   */
};

ra8_err_t ra8_board_led_pin(ra8_board_led_id_t led, ra8_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)led >= (uint8_t)k_ra8_board_led_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_pin = s_led_pins[led];
  return k_ra8_ok;
}

ra8_err_t ra8_board_led_init(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init(pin, k_ra8_level_low);
}

ra8_err_t ra8_board_led_on(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_write(pin, k_ra8_level_high);
}

ra8_err_t ra8_board_led_off(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_write(pin, k_ra8_level_low);
}

ra8_err_t ra8_board_led_toggle(ra8_board_led_id_t led)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_led_pin(led, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_toggle(pin);
}

/* =============================================================================
 * 2. User switches (UM Table 25, page 32)
 * =============================================================================
 */

/**
 * @brief Lookup table from ``ra8_board_sw_id_t`` to ``ra8_port_pin_t``.
 *
 * @details
 * SW1 -> P009, SW2 -> P008. Per UM Table 25 p 32.
 */
static const ra8_port_pin_t s_sw_pins[k_ra8_board_sw_count] = {
  [k_ra8_board_sw1] = (ra8_port_pin_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_9), /**< P009 / IRQ13-DS. */
  [k_ra8_board_sw2] = (ra8_port_pin_t)RA8_PIN(k_ra8_port_0, k_ra8_pin_8), /**< P008 / IRQ12-DS. */
};

/** @brief Lookup table from ``ra8_board_sw_id_t`` to ICU IRQ channel. */
static const uint8_t s_sw_irq_nums[k_ra8_board_sw_count] = {
  [k_ra8_board_sw1] = (uint8_t)k_ra8_board_sw1_irq, /**< 13 (IRQ13-DS). */
  [k_ra8_board_sw2] = (uint8_t)k_ra8_board_sw2_irq, /**< 12 (IRQ12-DS). */
};

ra8_err_t ra8_board_sw_pin(ra8_board_sw_id_t sw, ra8_port_pin_t* out_pin)
{
  if (out_pin == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  *out_pin = s_sw_pins[sw];
  return k_ra8_ok;
}

ra8_err_t ra8_board_sw_init(ra8_board_sw_id_t sw)
{
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_sw_pin(sw, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_input_init(pin, k_ra8_pull_up);
}

ra8_err_t ra8_board_sw_read(ra8_board_sw_id_t sw, ra8_board_sw_state_t* out_pressed)
{
  if (out_pressed == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  ra8_port_pin_t pin = k_ra8_pin_none;
  ra8_err_t      err = ra8_board_sw_pin(sw, &pin);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_level_t lvl = k_ra8_level_high;
  err             = ra8_gpio_read(pin, &lvl);
  if (err != k_ra8_ok) {
    /* ra8_gpio_read returns k_ra8_ok for valid port-0 SW pins in simulation. */
    return err; /* GCOVR_EXCL_LINE */
  }
  /* Buttons are active-low: low level == pressed. */
  *out_pressed = (lvl == k_ra8_level_low) ? k_ra8_board_sw_pressed : k_ra8_board_sw_released;
  return k_ra8_ok;
}

ra8_err_t ra8_board_sw_attach_irq(ra8_board_sw_id_t sw, ra8_board_sw_irq_cb_t cb, void* ctx)
{
  if (cb == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra8_board_sw_count) {
    return k_ra8_err_invalid_arg;
  }
  /* Step 1: configure the ICU IRQ pin for falling-edge detection so a
   * button press (active-low; UM Table 25 p 32) latches IRQCR[12] /
   * IRQCR[13]. The digital filter samples at PCLKB to debounce
   * contact bounce; HUM Ch 14.2.12 (p 535) describes IRQCRi fields. */
  const ra8_icu_irq_cfg_t cfg = {
    .sense      = k_ra8_icu_irqmd_falling,
    .filter_div = k_ra8_icu_fclksel_pclkb,
    .filter_en  = true,
  };
  ra8_err_t err = ra8_icu_configure_irq_pin(s_sw_irq_nums[sw], &cfg);
  if (err != k_ra8_ok) {
    /* ra8_icu_configure_irq_pin succeeds for IRQ12/13 (well within 32-channel limit). */
    return err; /* GCOVR_EXCL_LINE */
  }

  /* Step 2: route the ELC event for IRQ12-DS / IRQ13-DS through an
   * IELSR slot and enable the matching NVIC line. SW1 -> IRQ13-DS
   * (event 0x00E), SW2 -> IRQ12-DS (event 0x00D). */
  const ra8_elc_event_t evt =
    (sw == k_ra8_board_sw1) ? k_ra8_elc_event_icu_irq13 : k_ra8_elc_event_icu_irq12;
  return ra8_isr_register(evt, (ra8_isr_handler_t)cb, ctx, k_ra8_isr_prio_default, nullptr);
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
const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb888_pins[] = {
  {.signal = "BLEN",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_14)}, /**< J1-1 BLEN, P514. */
  {.signal = "SDA1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11)}, /**< J1-2 SDA1, P511. */
  {.signal = "INT",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_11)}, /**< J1-3 INT,  P111. */
  {.signal = "SCL1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12)}, /**< J1-4 SCL1, P512. */
  {.signal = "RST",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_6)}, /**< J1-6 RST,  P606. */
  {.signal = "TCON0",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_6)}, /**< J1-9 VSYNC/TCON0, P806. */
  {.signal = "CLK",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_15)}, /**< J1-10 CLK,        P515. */
  {.signal = "TCON2",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_7)}, /**< J1-11 DE/TCON2,   P807. */
  {.signal = "TCON1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_5)}, /**< J1-12 HSYNC/TCON1,P805. */
  {.signal = "EXTCLK",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_10)}, /**< J1-13 EXTCLK,     P710. */
  {.signal = "TCON3",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_13)}, /**< J1-14 TCON3,      P513. */
  {.signal = "B1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_15)}, /**< J1-15 DATA1/B1,   P915. */
  {.signal = "B0",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_14)}, /**< J1-16 DATA0/B0,   P914. */
  {.signal = "B3",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_2)}, /**< J1-17 DATA3/B3,   P902. */
  {.signal = "B2",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_3)}, /**< J1-18 DATA2/B2,   P903. */
  {.signal = "B5",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_11)}, /**< J1-19 DATA5/B5,   P911. */
  {.signal = "B4",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_10)}, /**< J1-20 DATA4/B4,   P910. */
  {.signal = "B7",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_13)}, /**< J1-21 DATA7/B7,   P913. */
  {.signal = "B6",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_12)}, /**< J1-22 DATA6/B6,   P912. */
  {.signal = "G1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_2, k_ra8_pin_7)}, /**< J1-23 DATA9/G1,   P207. */
  {.signal = "G0",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_4)}, /**< J1-24 DATA8/G0,   P904. */
  {.signal = "G3",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_6)}, /**< J1-25 DATA11/G3,  PB06. */
  {.signal = "G2",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_7)}, /**< J1-26 DATA10/G2,  PB07. */
  {.signal = "G5",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_1)}, /**< J1-27 DATA13/G5,  PB01. */
  {.signal = "G4",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_5)}, /**< J1-28 DATA12/G4,  PB05. */
  {.signal = "G7",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_3)}, /**< J1-29 DATA15/G7,  PB03. */
  {.signal = "G6",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_4)}, /**< J1-30 DATA14/G6,  PB04. */
  {.signal = "R1",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_0)}, /**< J1-31 DATA17/R1,  PB00. */
  {.signal = "R0",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_2)}, /**< J1-32 DATA16/R0,  PB02. */
  {.signal = "R3",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_11)}, /**< J1-33 DATA19/R3,  P711. */
  {.signal = "R2",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_7)}, /**< J1-34 DATA18/R2,  P707. */
  {.signal = "R5",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_13)}, /**< J1-35 DATA21/R5,  P713. */
  {.signal = "R4",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_12)}, /**< J1-36 DATA20/R4,  P712. */
  {.signal = "R7",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_15)}, /**< J1-37 DATA23/R7,  P715. */
  {.signal = "R6",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_14)}, /**< J1-38 DATA22/R6,  P714. */
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
const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb666_pins[] = {
  {.signal = "BLEN", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_14)},
  {.signal = "SDA1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11)},
  {.signal = "INT", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_11)},
  {.signal = "SCL1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12)},
  {.signal = "RST", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_6)},
  {.signal = "TCON0", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_6)},
  {.signal = "CLK", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_15)},
  {.signal = "TCON2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_7)},
  {.signal = "TCON1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_5)},
  {.signal = "EXTCLK", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_10)},
  {.signal = "TCON3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_13)},
  {.signal = "B3",
   .pin    = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_15)}, /**< J1-15 in RGB666. */
  {.signal = "B2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_14)},
  {.signal = "B5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_2)},
  {.signal = "B4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_3)},
  {.signal = "B7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_11)},
  {.signal = "B6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_10)},
  {.signal = "G3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_13)},
  {.signal = "G2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_12)},
  {.signal = "G5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_2, k_ra8_pin_7)},
  {.signal = "G4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_4)},
  {.signal = "G7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_6)},
  {.signal = "G6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_7)},
  {.signal = "R3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_1)},
  {.signal = "R2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_5)},
  {.signal = "R5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_3)},
  {.signal = "R4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_4)},
  {.signal = "R7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_0)},
  {.signal = "R6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_2)},
};

/**
 * @brief RGB565-mode pin table for J1 (UM Table 33 p 42).
 *
 * @details
 * RGB565 uses 5+6+5 = 16 active data lines. R6/R7 + G6/G7 are NC,
 * and DATA31..DATA38 (R0..R7 lines) are NC. Same control pins as
 * RGB888.
 */
const ra8_board_glcdc_pin_t g_ra8_board_glcdc_rgb565_pins[] = {
  {.signal = "BLEN", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_14)},
  {.signal = "SDA1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_11)},
  {.signal = "INT", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_1, k_ra8_pin_11)},
  {.signal = "SCL1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_12)},
  {.signal = "RST", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_6, k_ra8_pin_6)},
  {.signal = "TCON0", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_6)},
  {.signal = "CLK", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_15)},
  {.signal = "TCON2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_7)},
  {.signal = "TCON1", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_8, k_ra8_pin_5)},
  {.signal = "EXTCLK", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_7, k_ra8_pin_10)},
  {.signal = "TCON3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_5, k_ra8_pin_13)},
  {.signal = "B4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_15)},
  {.signal = "B3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_14)},
  {.signal = "B6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_2)},
  {.signal = "B5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_3)},
  {.signal = "G2", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_11)},
  {.signal = "B7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_10)},
  {.signal = "G4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_13)},
  {.signal = "G3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_12)},
  {.signal = "G6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_2, k_ra8_pin_7)},
  {.signal = "G5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_9, k_ra8_pin_4)},
  {.signal = "R3", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_6)},
  {.signal = "G7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_7)},
  {.signal = "R5", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_1)},
  {.signal = "R4", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_5)},
  {.signal = "R7", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_3)},
  {.signal = "R6", .pin = (ra8_port_pin_t)RA8_PIN(k_ra8_port_11, k_ra8_pin_4)},
};

const uint32_t g_ra8_board_glcdc_rgb888_pin_count =
  sizeof(g_ra8_board_glcdc_rgb888_pins) / sizeof(g_ra8_board_glcdc_rgb888_pins[0]);
const uint32_t g_ra8_board_glcdc_rgb666_pin_count =
  sizeof(g_ra8_board_glcdc_rgb666_pins) / sizeof(g_ra8_board_glcdc_rgb666_pins[0]);
const uint32_t g_ra8_board_glcdc_rgb565_pin_count =
  sizeof(g_ra8_board_glcdc_rgb565_pins) / sizeof(g_ra8_board_glcdc_rgb565_pins[0]);

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
RA8_INTERNAL static bool ra8_board_glcdc_signal_starts_with(const char* s, const char* prefix)
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
RA8_INTERNAL static bool ra8_board_glcdc_signal_is_color_data(const char* signal)
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
    /* No EK-RA8D2 GLCDC pin table entry starts with R/G/B followed by ASCII < '0'. */
    return false; /* GCOVR_EXCL_LINE */
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
 * `ra8_board_glcdc_init` to filter the J1 connector pin table down to
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
RA8_INTERNAL static bool ra8_board_glcdc_signal_is_output(const char* signal)
{
  if (ra8_board_glcdc_signal_starts_with(signal, "TCON")) {
    return true;
  }
  if (ra8_board_glcdc_signal_starts_with(signal, "CLK")) {
    return true;
  }
  return ra8_board_glcdc_signal_is_color_data(signal);
}

/**
 * @brief Force a PFS register to PSEL=glcdc, PMR=1, PDR=1 in one write.
 *
 * @details ``ra8_pfs_route_peripheral`` writes PSEL + PMR but leaves
 * PDR cleared, which keeps the pin in input direction.  GLCDC needs
 * its outputs in output direction; this helper does the combined
 * write directly under PWPR unlock so the chip drives the line.
 *
 * @param[in] pin J1 pin already validated to be a GLCDC output.
 *
 * @pre Caller has confirmed the pin is a GLCDC output via
 *      ``ra8_board_glcdc_signal_is_output``.
 * @pre IOPORT module is powered (true at reset).
 * @post PFS = (psel_glcdc << 24) | PMR | PDR for the target pin.
 * @post PWPR is left in its locked state.
 *
 * @note Single-threaded init context only.
 * @since 0.1.0
 */
RA8_INTERNAL static void ra8_board_glcdc_force_pin_output(ra8_port_pin_t pin)
{
  enum : uint32_t {
    k_pfs_psel_shift = 24U,      /**< PFS psel shift. */
    k_pfs_pmr_bit    = 1U << 16, /**< PFS pmr bit.    */
    k_pfs_pdr_bit    = 1U << 2,  /**< PFS pdr bit.    */
  };
  const ra8_port_t         port = RA8_PIN_PORT(pin);
  const ra8_pin_t          bit  = RA8_PIN_PIN(pin);
  volatile uint32_t* const pfs  = ra8_pfs_pmn(port, bit);
  if (pfs == nullptr) {
    /* ra8_pfs_pmn is non-null for every hardcoded board pin in the GLCDC table. */
    return; /* GCOVR_EXCL_LINE */
  }
  ra8_pfs_pwpr_unlock();
  *pfs = ((uint32_t)k_ra8_psel_glcdc << k_pfs_psel_shift) | k_pfs_pmr_bit | k_pfs_pdr_bit;
  ra8_pfs_pwpr_lock();
}

ra8_err_t ra8_board_glcdc_init(ra8_board_glcdc_fmt_t fmt)
{
  const ra8_board_glcdc_pin_t* table = nullptr;
  uint32_t                     count = 0U;

  switch (fmt) {
    case k_ra8_board_glcdc_fmt_rgb888:
      table = g_ra8_board_glcdc_rgb888_pins;
      count = g_ra8_board_glcdc_rgb888_pin_count;
      break;
    case k_ra8_board_glcdc_fmt_rgb666:
      table = g_ra8_board_glcdc_rgb666_pins;
      count = g_ra8_board_glcdc_rgb666_pin_count;
      break;
    case k_ra8_board_glcdc_fmt_rgb565:
      table = g_ra8_board_glcdc_rgb565_pins;
      count = g_ra8_board_glcdc_rgb565_pin_count;
      break;
    default:
      return k_ra8_err_invalid_arg;
  }

  for (uint32_t i = 0U; i < count; ++i) {
    if (!ra8_board_glcdc_signal_is_output(table[i].signal)) {
      continue; /* GPIO/I2C/clock-input -- not a GLCDC peripheral pin. */
    }
    const ra8_err_t err =
      ra8_pfs_route_peripheral(table[i].pin, k_ra8_psel_glcdc, "ra8_board.glcdc");
    if (err != k_ra8_ok) {
      return err;
    }
    /* Override PFS with PSEL + PMR + PDR=1 so the chip drives the
     * pin as an output instead of leaving it as an input under
     * peripheral control. */
    ra8_board_glcdc_force_pin_output(table[i].pin);
  }
  return k_ra8_ok;
}

/**
 * @brief Implementation of `ra8_board_lcd_panel_power_on`.
 *
 * @details See header for caller-facing contract.  Body drives
 * RESET_L low for 50 ms, releases it high for another 50 ms to let
 * the panel's internal POR finish, then asserts BLEN.
 *
 * @return Error code from the underlying GPIO operations.
 * @retval k_ra8_ok               Panel out of reset, backlight on.
 * @retval k_ra8_err_gpio_*       Underlying GPIO call failed.
 *
 * @pre IOPORT module powered (true at reset).
 * @pre `ra8_time_init` has been called.
 * @post P606 RESET_L is high; P514 BLEN is high.
 * @post Returns within ~100 ms.
 *
 * @note Single-threaded init context; blocks on `ra8_delay_ms`.
 * @since 0.1.0
 */
ra8_err_t ra8_board_lcd_panel_power_on(void)
{
  enum : uint32_t {
    k_reset_pulse_ms = 50U, /**< Reset pulse ms. */
  };
  ra8_err_t err = ra8_gpio_output_init(k_ra8_pin_lcd_reset_l, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms(k_reset_pulse_ms);
  err = ra8_gpio_write(k_ra8_pin_lcd_reset_l, k_ra8_level_high);
  if (err != k_ra8_ok) {
    /* ra8_gpio_write returns k_ra8_ok for valid port/pin pairs in simulation. */
    return err; /* GCOVR_EXCL_LINE */
  }
  ra8_delay_ms(k_reset_pulse_ms);
  return ra8_gpio_output_init(k_ra8_pin_lcd_blen, k_ra8_level_high);
}

ra8_err_t ra8_board_backlight_set(bool on)
{
  /* BLEN (P514) is already a GPIO output after ra8_board_lcd_panel_power_on;
   * drive it high to light the panel backlight, low to blank it. Active-high,
   * so `on` maps directly to the output level. */
  return ra8_gpio_write(k_ra8_pin_lcd_blen, on ? k_ra8_level_high : k_ra8_level_low);
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
 * of (port << 8) | pin -- compatible with ``ra8_port_pin_t`` value
 * space. The RESET_L pin is intentionally NOT in this table: it is
 * driven by the GPIO subsystem (active-low strap, not a peripheral
 * function). Source: EK-RA8D2 UM Table 29 "Octo-SPI Flash
 * Assignments" p 35.
 *
 * The ``ra8_port_pin_t`` enum only enumerates the convenience
 * ``k_ra8_pin_led*`` aliases; raw RA8_PIN()-derived values are valid
 * data-space members but trigger the EnumCastOutOfRange checker, so
 * the cast cluster is wrapped in a NOLINT region matching the
 * board-pin convention used throughout this BSP.
 */
/* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
static const ra8_port_pin_t s_xspi_octa_pins[] = {
  (ra8_port_pin_t)k_ra8_board_xspi_cs,  /**< OSPI_FLASH_S_L,   P104. */
  (ra8_port_pin_t)k_ra8_board_xspi_clk, /**< OSPI_FLASH_C,     P808. */
  (ra8_port_pin_t)k_ra8_board_xspi_dqs, /**< OSPI_FLASH_DQS,   P801. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq0, /**< OSPI_FLASH_DQ0,   P100. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq1, /**< OSPI_FLASH_DQ1,   P803. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq2, /**< OSPI_FLASH_DQ2,   P103. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq3, /**< OSPI_FLASH_DQ3,   P101. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq4, /**< OSPI_FLASH_DQ4,   P102. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq5, /**< OSPI_FLASH_DQ5,   P800. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq6, /**< OSPI_FLASH_DQ6,   P802. */
  (ra8_port_pin_t)k_ra8_board_xspi_dq7, /**< OSPI_FLASH_DQ7,   P804. */
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
 * format then bailed at ``ra8_xspi_flash_read_id``. Post-release
 * settle is now 15 ms, comfortably clearing tPUW from a true cold
 * boot. The pre-release low pulse stays at 1 ms (still 10000x the
 * 100 ns minimum tRLRH, just generous).
 */
typedef enum : uint32_t {
  k_ra8_board_xspi_reset_low_ms  = 1U,  /**< Hold RESET_L low for >= tRLRH. */
  k_ra8_board_xspi_reset_high_ms = 15U, /**< Wait >= tPUW before first CS.  */
} ra8_board_xspi_reset_timing_t;

ra8_err_t ra8_board_xspi_pins_init(void)
{
  /* Step 1: drive RESET_L low while the pin is still GPIO. PSEL=0x00
   * + PDR=output is the IS25LX512M strap that holds the chip in
   * reset (datasheet Ch 9.2). EK-RA8D2 UM Table 29 maps RESET_L to
   * P106. */
  /* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
  const ra8_port_pin_t reset_pin = (ra8_port_pin_t)k_ra8_board_xspi_reset;
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  ra8_err_t err = ra8_gpio_output_init(reset_pin, k_ra8_level_low);
  if (err != k_ra8_ok) {
    return err;
  }
  ra8_delay_ms((uint32_t)k_ra8_board_xspi_reset_low_ms);

  /* Step 2: drive RESET_L high to release the chip. */
  err = ra8_gpio_write(reset_pin, k_ra8_level_high);
  if (err != k_ra8_ok) {
    /* ra8_gpio_write returns k_ra8_ok for valid port/pin pairs in simulation. */
    return err; /* GCOVR_EXCL_LINE */
  }
  ra8_delay_ms((uint32_t)k_ra8_board_xspi_reset_high_ms);

  /* Step 3: route the 12 OCTA bus pins to PSEL=0x1C. PSEL 0x1C is
   * shared between the QSPI and Octo-SPI controllers per HUM Ch 20.6
   * "Multiplexed Pin Function Selector" p 871; the named constant
   * is ``k_ra8_psel_qspi`` for legacy reasons. */
  const uint32_t count = (uint32_t)(sizeof(s_xspi_octa_pins) / sizeof(s_xspi_octa_pins[0]));
  for (uint32_t i = 0U; i < count; ++i) {
    err = ra8_pfs_route_peripheral(s_xspi_octa_pins[i], k_ra8_psel_qspi, "ra8_board.xspi");
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * 6d. Native SDHI bus pin routing (SDHI0, port 4 pins 0..7)
 * =============================================================================
 */

/**
 * @brief Lookup table for the eight SDHI0 bus pins routed to PSEL=0x15.
 *
 * @details
 * Bus order: CMD, CLK, DAT0..DAT3, WP, CD. The pin enums are uint16_t
 * encodings of (port << 8) | pin -- compatible with the ``ra8_port_pin_t``
 * value space. Source: chip HUM Ch 20.6 "Multiplexed Pin Function
 * Selector" SDHI pin group (the EK-RA8D2 v1 board has no on-board
 * micro-SD socket -- see the @warning on ::ra8_board_sdhi_pin_t).
 *
 * The ``ra8_port_pin_t`` enum only enumerates the convenience
 * ``k_ra8_pin_led*`` aliases; raw RA8_PIN()-derived values are valid
 * data-space members but trigger the EnumCastOutOfRange checker, so the
 * cast cluster is wrapped in a NOLINT region matching the board-pin
 * convention used throughout this BSP.
 */
/* NOLINTBEGIN(clang-analyzer-optin.core.EnumCastOutOfRange) -- RA8_PIN()-packed board pin is valid ra8_port_pin_t data outside the enumerator list. */
static const ra8_port_pin_t s_sdhi_bus_pins[] = {
  (ra8_port_pin_t)k_ra8_board_sdhi_cmd,  /**< SDHI0 CMD,  P400. */
  (ra8_port_pin_t)k_ra8_board_sdhi_clk,  /**< SDHI0 CLK,  P401. */
  (ra8_port_pin_t)k_ra8_board_sdhi_dat0, /**< SDHI0 DAT0, P402. */
  (ra8_port_pin_t)k_ra8_board_sdhi_dat1, /**< SDHI0 DAT1, P403. */
  (ra8_port_pin_t)k_ra8_board_sdhi_dat2, /**< SDHI0 DAT2, P404. */
  (ra8_port_pin_t)k_ra8_board_sdhi_dat3, /**< SDHI0 DAT3, P405. */
  (ra8_port_pin_t)k_ra8_board_sdhi_wp,   /**< SDHI0 WP,   P406. */
  (ra8_port_pin_t)k_ra8_board_sdhi_cd,   /**< SDHI0 CD,   P407. */
};
/* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */

ra8_err_t ra8_board_sdhi_pins_init(void)
{
  /* Route the eight SDHI0 bus pins to PSEL=0x15. PSEL 0x15 is the SDHI
   * SD / MMC function per chip HUM Ch 20.6 "Multiplexed Pin Function
   * Selector"; the named constant is ``k_ra8_psel_sdhi``. */
  const uint32_t count = (uint32_t)(sizeof(s_sdhi_bus_pins) / sizeof(s_sdhi_bus_pins[0]));
  for (uint32_t i = 0U; i < count; ++i) {
    const ra8_err_t err =
      ra8_pfs_route_peripheral(s_sdhi_bus_pins[i], k_ra8_psel_sdhi, "ra8_board.sdhi");
    if (err != k_ra8_ok) {
      return err;
    }
  }
  return k_ra8_ok;
}

/* =============================================================================
 * 5. Arduino header (UM Table 20, page 28)
 * =============================================================================
 */

ra8_err_t ra8_board_arduino_pin_init(ra8_board_arduino_pin_t pin, ra8_board_arduino_mode_t mode)
{
  switch (mode) {
    case k_ra8_board_arduino_mode_input:
      return ra8_gpio_input_init((ra8_port_pin_t)pin, k_ra8_pull_none);
    case k_ra8_board_arduino_mode_input_pullup:
      return ra8_gpio_input_init((ra8_port_pin_t)pin, k_ra8_pull_up);
    case k_ra8_board_arduino_mode_output:
      return ra8_gpio_output_init((ra8_port_pin_t)pin, k_ra8_level_low);
    default:
      return k_ra8_err_invalid_arg;
  }
}

ra8_err_t ra8_board_arduino_gpio_write(ra8_board_arduino_pin_t pin, ra8_level_t level)
{
  return ra8_gpio_write((ra8_port_pin_t)pin, level);
}

ra8_err_t ra8_board_arduino_gpio_read(ra8_board_arduino_pin_t pin, ra8_level_t* out_level)
{
  if (out_level == nullptr) {
    return k_ra8_err_invalid_arg;
  }
  return ra8_gpio_read((ra8_port_pin_t)pin, out_level);
}
