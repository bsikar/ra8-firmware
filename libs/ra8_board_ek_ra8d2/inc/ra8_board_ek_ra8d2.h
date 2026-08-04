/**
 * @file ra8_board_ek_ra8d2.h
 * @brief Board-support layer for the Renesas EK-RA8D2 v1 evaluation kit
 * @ingroup grp_board
 *
 * @par Tag
 * [Ring 5 / BSP] {World: S}
 *
 * @details
 * Names every on-board feature of the EK-RA8D2 v1 (LEDs, switches,
 * connectors, peripherals) so application code can speak in board
 * coordinates ("LED1", "Arduino D13", "Pmod1 SCK") rather than chip
 * coordinates ("P600", "P102", "P803"). Every pin enum carries a
 * citation back to a numbered table in the EK-RA8D2 v1 User's Manual
 * so a reader can audit the wiring without opening the schematic.
 *
 * This header is a THIN UMBRELLA: the declarations themselves live in
 * two self-contained sub-headers in this directory, and this file just
 * pulls them in so existing consumers that ``#include
 * "ra8_board_ek_ra8d2.h"`` keep compiling unchanged:
 *
 *   - ``ra8_board_ek_ra8d2_connectors.h`` -- board identity, user LEDs,
 *     user switches, parallel-RGB J1, audio CODEC, Arduino header,
 *     Pmod1/Pmod2, MikroBUS, and the project SW4-layout enum.
 *   - ``ra8_board_ek_ra8d2_peripherals.h`` -- the U15 I/O-expander SW4
 *     override functions, USB-HS/FS, parallel camera J35, Octo-SPI flash
 *     + SDRAM, MIPI-DSI J32, the J-Link OB VCOM console, and Ethernet.
 *
 * Authoritative source: ``docs/reference/ek-ra8d2-v1-users-manual.pdf``
 * (Rev 1.01, R20UT5523EG0101, October 2025).
 *
 * Underlying chip register access is delegated to ``libs/ra8_hal``:
 * the BSP itself does NO register pokes -- it just translates board
 * names into the right ``ra8_port_pin_t`` / ``ra8_psel_t`` /
 * ``ra8_icu_irq_cfg_t`` values and forwards to the HAL.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#include "ra8_board_ek_ra8d2_connectors.h"
#include "ra8_board_ek_ra8d2_peripherals.h"
