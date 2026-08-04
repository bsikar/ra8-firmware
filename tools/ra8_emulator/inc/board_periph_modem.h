/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_modem.h
 * @brief Cellular AT-modem device model for ra8_emulator (attached to SCI7 UART).
 *
 * @details
 * Models a 3GPP AT-command cellular modem (SIM7600 / Quectel BG95 class) wired
 * to the EK-RA8D2 MikroBUS UART -- RXD7 / TXD7, i.e. SCI channel 7
 * (``k_ra8_board_mikrobus_uart_sci_channel``). A MikroE cellular Click
 * (LTE IoT / 4G LTE / NB-IoT) presents its modem UART on exactly these pads.
 *
 * The model is transport-symmetric with ``board_periph_sd.c`` /
 * ``board_periph_eink.c``: it is attached with ``--modem`` and one seam ties it
 * into the rest of the emulator. The SCI_B block model
 * (``board_periph_sci.c``) routes every TDR byte the firmware writes on the
 * modem channel into ::board_modem_feed_tx; the model accumulates a command
 * line, and when the terminating ``\r`` arrives it answers with the exact bytes
 * a real modem would send, which the SCI block pushes back into that channel's
 * RX queue (RDR / RDRF). The genuine firmware path --
 * ``ra8_modem_at`` over ``ra8_sci`` polled TX/RX -- then runs byte for byte, so
 * the ``modem_at_demo`` example brings the modem up, walks its AT state machine
 * (sync / SIM / signal / registration / attach), dispatches a ``+CREG`` URC and
 * exercises the ``+CME ERROR`` path with no physical modem (EIL == HIL for the
 * protocol; the physical RF link is the only unmodelled part, hence the app is
 * hw_pending).
 *
 * The AT script answered here mirrors what the demo sends; an unrecognised
 * command is rejected with ``+CME ERROR: 4`` exactly as a modem with
 * ``AT+CMEE=1`` active would, so the demo's error branch is faithful too.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Attach (arm) the modelled AT cellular modem.
 *
 * @details
 * Sets the module's armed flag so ::board_modem_attached returns true and the
 * SCI_B block routes the modem channel's TDR writes into ::board_modem_feed_tx.
 * Idempotent: a second call re-arms and clears the parser state.
 *
 * @return true once the modem is armed and answering.
 * @retval true Modem armed (always, on this in-memory model).
 *
 * @pre Called once during ra8_emulator start-up (single-threaded arg parse).
 * @pre No physical hardware is required.
 * @post ::board_modem_attached returns true.
 * @post The command accumulator and response FIFO are empty.
 *
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
bool board_modem_attach(void);

/**
 * @brief Report whether the AT modem model is currently attached.
 *
 * @return true if ``--modem`` armed the model.
 * @retval false No ``--modem`` was passed.
 *
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 *
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
bool board_modem_attached(void);

/**
 * @brief SCI channel the modem is wired to (RXD7 / TXD7 = SCI7).
 *
 * @details Matches ``k_ra8_board_mikrobus_uart_sci_channel`` on the firmware
 * side so the SCI_B block routes only that channel's bytes into the model.
 *
 * @return The modem's SCI channel number (7).
 * @retval 7 Always, for the EK-RA8D2 MikroBUS UART mapping.
 *
 * @pre None.
 * @pre None.
 * @post No state is modified.
 * @post No state is modified.
 *
 * @since 0.1.0
 */
uint8_t board_modem_channel(void);

/**
 * @brief Feed one firmware-transmitted byte into the modem and drain a reply.
 *
 * @details
 * Appends @p tx to the command-line accumulator. Most bytes buffer silently and
 * return 0; the terminating ``\r`` closes the line, looks up the scripted
 * response for that AT command, copies it into @p out (bounded by @p out_cap)
 * and returns its length. An unrecognised command yields ``+CME ERROR: 4``.
 * A command whose response also stages an unsolicited ``+CREG`` URC (the
 * ``AT+CREG=1`` enable) appends that URC after the ``OK`` so the demo's
 * ``ra8_modem_at_poll`` dispatch fires exactly as on silicon.
 *
 * @param[in]  tx      The byte the firmware wrote to the modem-channel TDR.
 * @param[out] out     Destination for any response bytes (may stay untouched).
 * @param[in]  out_cap Capacity of @p out in bytes.
 * @return Number of response bytes written to @p out (0 if none this byte).
 * @retval 0 The byte was buffered; no complete line yet.
 *
 * @pre @p out is non-null and points to at least @p out_cap bytes.
 * @pre The model is attached (callers guard with ::board_modem_attached).
 * @post On a completed line @p out holds a full modem response (<= @p out_cap).
 * @post The command accumulator is reset after a completed line.
 *
 * @note Not thread-safe; ra8_emulator is single-threaded.
 * @since 0.1.0
 */
uint32_t board_modem_feed_tx(uint8_t tx, uint8_t* out, uint32_t out_cap);

/**
 * @brief Clear the modem's parser state (keeps the attached flag).
 *
 * @details Called from the SCI block reset so a mid-run reset re-frames the
 * command accumulator without detaching the modem (the SCI reset re-frames
 * transport, not the modem's presence). Run counters are retained for the
 * end-of-run report.
 *
 * @pre None.
 * @pre None.
 * @post The command accumulator is empty; the attached flag is unchanged.
 * @post Run counters (commands answered, errors) are retained.
 *
 * @since 0.1.0
 */
void board_modem_reset(void);

/**
 * @brief Print a one-line end-of-run summary if the modem was exercised.
 *
 * @pre None.
 * @pre None.
 * @post One line is written to stderr iff at least one command was answered.
 * @post No state is modified.
 *
 * @since 0.1.0
 */
void board_modem_report(void);

#ifdef __cplusplus
}
#endif
