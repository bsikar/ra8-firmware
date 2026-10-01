/**
 * @file ra8_board_ek_ra8d2_pmod.c
 * @brief Pmod2 (J25) Simple-SPI bus bring-up for the EK-RA8D2 v1 board.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Implements ``ra8_board_ek_ra8d2_pmod.h``. Pure translation: board pin
 * names into ``ra8_pfs`` / ``ra8_gpio`` calls, no register pokes of its
 * own, the same contract every other file in this layer keeps.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "ra8_board_ek_ra8d2_pmod.h"

#include <stdbool.h>

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_err.h"
#include "ra8_gpio_constants.h"
#include "ra8_port_constants.h"
#include "ra8_port_utils.h"

ra8_err_t ra8_board_pmod2_spi_bus_init(void)
{
  ra8_err_t err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod2_spi_sck,
                                           k_ra8_psel_sci_async,
                                           "pmod2.sck");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod2_spi_cipo,
                                 k_ra8_psel_sci_async,
                                 "pmod2.cipo");
  if (err != k_ra8_ok) {
    return err;
  }
  err = ra8_pfs_route_peripheral((ra8_port_pin_t)k_ra8_board_pmod2_spi_copi,
                                 k_ra8_psel_sci_async,
                                 "pmod2.copi");
  if (err != k_ra8_ok) {
    return err;
  }
  return ra8_gpio_output_init((ra8_port_pin_t)k_ra8_board_pmod2_spi_cs, k_ra8_level_high);
}

ra8_err_t ra8_board_pmod2_spi_cs_set(bool asserted)
{
  return ra8_gpio_write((ra8_port_pin_t)k_ra8_board_pmod2_spi_cs,
                        asserted ? k_ra8_level_low : k_ra8_level_high);
}
