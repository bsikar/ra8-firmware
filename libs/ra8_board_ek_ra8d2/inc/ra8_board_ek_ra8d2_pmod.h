/**
 * @file ra8_board_ek_ra8d2_pmod.h
 * @brief Pmod2 (J25) Simple-SPI bus bring-up for the EK-RA8D2 v1 board.
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Sub-header of ``ra8_board_ek_ra8d2.h`` (the thin umbrella). The pin
 * names themselves live in ``ra8_board_ek_ra8d2_connectors.h`` as
 * ::ra8_board_pmod2_spi_pin_t; this header adds the two routing calls
 * every Pmod2 SPI consumer was writing out by hand.
 *
 * Seven example apps (the microSD-over-Pmod2 family) each declared their
 * own copies of the four P601..P604 pin constants, routed SCK/CIPO/COPI
 * to ``k_ra8_psel_sci_async`` in their own three-step helper, claimed
 * P604 as a GPIO output, and wrote the same active-low chip-select
 * callback. That is one board routing fact spread over seven files, and
 * it is what ``ra8_board_pmod2_spi_bus_init`` and
 * ``ra8_board_pmod2_spi_cs_set`` replace.
 *
 * The bus is SCI0 in Simple-SPI mode, not RSPI -- see the long note on
 * ::ra8_board_pmod2_spi_pin_t for why. Pick the controller channel up
 * from ::k_ra8_board_pmod2_sci_channel; this header only owns the pins.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025), Table 19 p 27.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include <stdbool.h>

#include "ra8_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Route the Pmod2 (J25) Simple-SPI pins and park chip-select high.
 *
 * @details
 * Routes P601 (SCK), P602 (CIPO) and P603 (COPI) to the SCI0 function at
 * ``k_ra8_psel_sci_async``, then claims P604 (CS) as a GPIO output driven
 * high, which is the idle level for the active-low select. Call it once,
 * before ``ra8_sci_spi_init`` on ::k_ra8_board_pmod2_sci_channel.
 *
 * Chip-select is deliberately a GPIO rather than the SCI's own SS0_B
 * function: an SD card in SPI mode needs CS held low across a whole
 * multi-byte command-and-response exchange, which hardware slave-select
 * does not do.
 *
 * @return ra8_err_t from the first routing call that fails.
 * @retval k_ra8_ok     All four pins routed.
 * @retval k_ra8_err_*  Propagated from ``ra8_pfs_route_peripheral`` or
 *                      ``ra8_gpio_output_init``.
 * @pre IOPORT is reachable and the Pmod2 jumpers select SPI signalling.
 * @post P601..P603 carry SCI0 Simple-SPI; P604 is a GPIO output, high.
 * @note Not thread-safe; call once during board bring-up.
 * @see ra8_board_pmod2_spi_cs_set
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pmod2_spi_bus_init(void);

/**
 * @brief Drive the Pmod2 (J25) chip-select, active low.
 *
 * @param[in] asserted ``true`` selects the device (P604 low), ``false``
 *                     releases it (P604 high).
 *
 * @return ra8_err_t from the underlying GPIO write.
 * @retval k_ra8_ok     Level driven.
 * @retval k_ra8_err_*  Propagated from ``ra8_gpio_write``.
 * @pre ``ra8_board_pmod2_spi_bus_init`` has claimed P604 as an output.
 * @note Signature matches ``ra8_sdmmc_spi_transport_t::cs`` once wrapped
 *       in a one-line adapter that drops the unused context pointer.
 * @see ra8_board_pmod2_spi_bus_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_board_pmod2_spi_cs_set(bool asserted);

#ifdef __cplusplus
}
#endif
