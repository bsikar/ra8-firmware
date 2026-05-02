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

#include "ra_err.h"
#include "ra_gpio_constants.h"
#include "ra_port_constants.h"
#include "ra_port_utils.h"

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

ra_err_t
ra_board_sw_attach_irq(ra_board_sw_id_t sw, ra_board_sw_irq_cb_t cb, void* ctx)
{
  if (cb == NULL) {
    return k_ra_err_invalid_arg;
  }
  if ((uint8_t)sw >= (uint8_t)k_ra_board_sw_count) {
    return k_ra_err_invalid_arg;
  }
  /* The actual ICU IRQ wiring is left to the caller's HAL config; this
   * BSP veneer only verifies the (sw, irq) mapping above and stages
   * the callback. The HAL hook lands once ra_icu_register_callback
   * is wired through ra_board_sw_attach_irq -- left as a TODO since
   * the ICU callback table is module-internal in libs/ra_hal/src.
   *
   * TODO(bsp): once libs/ra_hal exposes ra_icu_register_callback for
   * external IRQ channels, plumb (s_sw_irq_nums[sw], cb, ctx) here.
   * Today we silence "unused" warnings to keep the BSP compiling. */
  (void)s_sw_irq_nums[sw];
  (void)cb;
  (void)ctx;
  return k_ra_err_not_supported;
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
  {.signal = "BLEN",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)},  /**< J1-1 BLEN, P514. */
  {.signal = "SDA1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)},  /**< J1-2 SDA1, P511. */
  {.signal = "INT",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)},  /**< J1-3 INT,  P111. */
  {.signal = "SCL1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},  /**< J1-4 SCL1, P512. */
  {.signal = "RST",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)},   /**< J1-6 RST,  P606. */
  {.signal = "TCON0",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)},   /**< J1-9 VSYNC/TCON0, P806. */
  {.signal = "CLK",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)},  /**< J1-10 CLK,        P515. */
  {.signal = "TCON2",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)},   /**< J1-11 DE/TCON2,   P807. */
  {.signal = "TCON1",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)},   /**< J1-12 HSYNC/TCON1,P805. */
  {.signal = "EXTCLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)},  /**< J1-13 EXTCLK,     P710. */
  {.signal = "TCON3",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)},  /**< J1-14 TCON3,      P513. */
  {.signal = "B1",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)},  /**< J1-15 DATA1/B1,   P915. */
  {.signal = "B0",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)},  /**< J1-16 DATA0/B0,   P914. */
  {.signal = "B3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)},   /**< J1-17 DATA3/B3,   P902. */
  {.signal = "B2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)},   /**< J1-18 DATA2/B2,   P903. */
  {.signal = "B5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)},  /**< J1-19 DATA5/B5,   P911. */
  {.signal = "B4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)},  /**< J1-20 DATA4/B4,   P910. */
  {.signal = "B7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)},  /**< J1-21 DATA7/B7,   P913. */
  {.signal = "B6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)},  /**< J1-22 DATA6/B6,   P912. */
  {.signal = "G1",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)},   /**< J1-23 DATA9/G1,   P207. */
  {.signal = "G0",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)},   /**< J1-24 DATA8/G0,   P904. */
  {.signal = "G3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)},  /**< J1-25 DATA11/G3,  PB06. */
  {.signal = "G2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)},  /**< J1-26 DATA10/G2,  PB07. */
  {.signal = "G5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)},  /**< J1-27 DATA13/G5,  PB01. */
  {.signal = "G4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)},  /**< J1-28 DATA12/G4,  PB05. */
  {.signal = "G7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)},  /**< J1-29 DATA15/G7,  PB03. */
  {.signal = "G6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)},  /**< J1-30 DATA14/G6,  PB04. */
  {.signal = "R1",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_0)},  /**< J1-31 DATA17/R1,  PB00. */
  {.signal = "R0",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_2)},  /**< J1-32 DATA16/R0,  PB02. */
  {.signal = "R3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_11)},  /**< J1-33 DATA19/R3,  P711. */
  {.signal = "R2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_7)},   /**< J1-34 DATA18/R2,  P707. */
  {.signal = "R5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_13)},  /**< J1-35 DATA21/R5,  P713. */
  {.signal = "R4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_12)},  /**< J1-36 DATA20/R4,  P712. */
  {.signal = "R7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_15)},  /**< J1-37 DATA23/R7,  P715. */
  {.signal = "R6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_14)},  /**< J1-38 DATA22/R6,  P714. */
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
  {.signal = "BLEN",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)},
  {.signal = "SDA1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)},
  {.signal = "INT",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)},
  {.signal = "SCL1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},
  {.signal = "RST",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)},
  {.signal = "TCON0",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)},
  {.signal = "CLK",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)},
  {.signal = "TCON2",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)},
  {.signal = "TCON1",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)},
  {.signal = "EXTCLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)},
  {.signal = "TCON3",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)},
  {.signal = "B3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)},  /**< J1-15 in RGB666. */
  {.signal = "B2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)},
  {.signal = "B5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)},
  {.signal = "B4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)},
  {.signal = "B7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)},
  {.signal = "B6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)},
  {.signal = "G3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)},
  {.signal = "G2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)},
  {.signal = "G5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)},
  {.signal = "G4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)},
  {.signal = "G7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)},
  {.signal = "G6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)},
  {.signal = "R3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)},
  {.signal = "R2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)},
  {.signal = "R5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)},
  {.signal = "R4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)},
  {.signal = "R7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_0)},
  {.signal = "R6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_2)},
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
  {.signal = "BLEN",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_14)},
  {.signal = "SDA1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_11)},
  {.signal = "INT",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_1, k_ra_pin_11)},
  {.signal = "SCL1",   .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_12)},
  {.signal = "RST",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_6, k_ra_pin_6)},
  {.signal = "TCON0",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_6)},
  {.signal = "CLK",    .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_15)},
  {.signal = "TCON2",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_7)},
  {.signal = "TCON1",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_8, k_ra_pin_5)},
  {.signal = "EXTCLK", .pin = (ra_port_pin_t)RA_PIN(k_ra_port_7, k_ra_pin_10)},
  {.signal = "TCON3",  .pin = (ra_port_pin_t)RA_PIN(k_ra_port_5, k_ra_pin_13)},
  {.signal = "B4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_15)},
  {.signal = "B3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_14)},
  {.signal = "B6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_2)},
  {.signal = "B5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_3)},
  {.signal = "G2",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_11)},
  {.signal = "B7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_10)},
  {.signal = "G4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_13)},
  {.signal = "G3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_12)},
  {.signal = "G6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_2, k_ra_pin_7)},
  {.signal = "G5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_9, k_ra_pin_4)},
  {.signal = "R3",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_6)},
  {.signal = "G7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_7)},
  {.signal = "R5",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_1)},
  {.signal = "R4",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_5)},
  {.signal = "R7",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_3)},
  {.signal = "R6",     .pin = (ra_port_pin_t)RA_PIN(k_ra_port_11, k_ra_pin_4)},
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
    const ra_err_t err =
      ra_pfs_route_peripheral(table[i].pin, k_ra_psel_glcdc, "ra_board.glcdc");
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
    {(ra_port_pin_t)k_ra_board_audio_pin_bclk,    k_ra_psel_iic,        "ra_board.audio.bclk"   },
    {(ra_port_pin_t)k_ra_board_audio_pin_wclk,    k_ra_psel_iic,        "ra_board.audio.wclk"   },
    {(ra_port_pin_t)k_ra_board_audio_pin_datin,   k_ra_psel_iic,        "ra_board.audio.datin"  },
    {(ra_port_pin_t)k_ra_board_audio_pin_datout,  k_ra_psel_iic,        "ra_board.audio.datout" },
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_sda, k_ra_psel_iic,        "ra_board.audio.i2c.sda"},
    {(ra_port_pin_t)k_ra_board_audio_pin_i2c_scl, k_ra_psel_iic,        "ra_board.audio.i2c.scl"},
  };
  /* NOLINTEND(clang-analyzer-optin.core.EnumCastOutOfRange) */
  /* TODO(bsp): k_ra_psel_iic is the IIC alt; SSIE uses a different
   * PSEL value not currently named in ra_gpio_constants.h. Once
   * k_ra_psel_ssie is added, swap the four DAI entries above. */
  for (uint32_t i = 0U; i < sizeof(routes) / sizeof(routes[0]); ++i) {
    const ra_err_t err =
      ra_pfs_route_peripheral(routes[i].pin, routes[i].psel, routes[i].owner);
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
  /* TODO(bsp): wire to ra_ssie_write_buffer once SSIE callback
   * registration on EK-RA8D2 has been validated. */
  return k_ra_err_not_supported;
}

/* =============================================================================
 * 5. Arduino header (UM Table 20, page 28)
 * =============================================================================
 */

ra_err_t
ra_board_arduino_pin_init(ra_board_arduino_pin_t pin, ra_board_arduino_mode_t mode)
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

ra_err_t ra_board_usbhs_device_init(void)
{
  /* TODO(bsp): integrate ra_usb_pal HS device init once the PHY
   * power sequence is validated against UM section 6.2 + chip HUM. */
  return k_ra_err_not_supported;
}

ra_err_t ra_board_usbhs_host_init(void)
{
  /* TODO(bsp): integrate ra_usb_pal HS host init once VBUS-enable
   * routing on EK-RA8D2 is itemised in a future UM revision. */
  return k_ra_err_not_supported;
}

/* =============================================================================
 * 10. MIPI-DSI bring-up stub
 * =============================================================================
 */

ra_err_t ra_board_mipi_dsi_init(void)
{
  /* TODO(bsp): wire to ra_mipi_dsi_init() once a validated config
   * for the MIPI Graphics Expansion Board 1 panel
   * (E45RA-MW276-C, 854 x 480) is committed under cmake/. */
  return k_ra_err_not_supported;
}
