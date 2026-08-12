/**
 * @file board_periph_modem.c
 * @brief Cellular AT-modem device model for ra8_emulator (attached to SCI7).
 *
 * @details
 * Implements board_periph_modem.h. The modem is a line state machine on the
 * SCI7 UART: it accumulates the bytes the ``ra8_modem_at`` driver clocks out
 * one at a time (each a TDR write the SCI_B block routes here), and when the
 * terminating ``\r`` arrives it answers with the exact byte stream a real
 * SIM7600 / BG95-class modem would send. The SCI_B block pushes that stream
 * back into the channel's RX queue (RDR / RDRF), so the firmware's genuine
 * polled ``ra8_sci_getc_polling`` path drains it exactly as on silicon
 * (EIL == HIL for the AT protocol).
 *
 * The AT script answered here mirrors the ``modem_at_demo`` example. An
 * unrecognised command is rejected with ``+CME ERROR: 4`` ("operation not
 * supported") exactly as a modem with ``AT+CMEE=1`` active would, so the demo's
 * error branch is faithful. ``AT+CREG=1`` (enable registration URCs) answers
 * ``OK`` and then stages an unsolicited ``+CREG: 1`` so the demo's URC dispatch
 * fires, matching a modem reporting its current registration on enable.
 *
 * Self-registers nothing with the peripheral core -- it is a pure device model
 * the SCI_B block calls into (attach / attached / channel / feed_tx / reset /
 * report), the same shape as board_periph_sd.c and board_periph_eink.c.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include "board_periph_modem.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

/**
 * @enum modem_geom_t
 * @brief Sizing constants for the modem line model (no magic numbers).
 */
typedef enum : uint32_t {
  k_modem_channel  = 7U,  /**< RXD7/TXD7 = SCI7 (MikroBUS UART).  */
  k_modem_cmd_cap  = 64U, /**< Longest AT command line accepted.  */
  k_modem_resp_cap = 96U, /**< Longest scripted response.         */
  k_modem_cr       = 13U, /**< Carriage return (line terminator). */
  k_modem_lf       = 10U, /**< Line feed (dropped from the line). */
} modem_geom_t;

/**
 * @struct modem_reply_t
 * @brief One scripted (command -> response) pair.
 */
typedef struct {
  const char* cmd;  /**< AT command WITHOUT the trailing CR.    */
  const char* resp; /**< Bytes the modem returns (CRLF-framed). */
} modem_reply_t;

/**
 * @brief The AT script the modelled modem answers.
 *
 * @details
 * Responses are framed the way a real modem frames them: a leading ``\r\n``,
 * the payload line(s), a blank line, then the final ``OK``. ``AT+CREG=1``
 * additionally stages an unsolicited ``+CREG: 1`` after its ``OK`` (the modem
 * reporting current registration once URCs are enabled). Any command not in
 * this table falls through to ``+CME ERROR: 4``.
 */
static const modem_reply_t k_modem_script[] = {
  {"AT", "\r\nOK\r\n"},
  {"ATE0", "\r\nOK\r\n"},
  {"AT+CMEE=1", "\r\nOK\r\n"},
  {"AT+CPIN?", "\r\n+CPIN: READY\r\n\r\nOK\r\n"},
  {"AT+CSQ", "\r\n+CSQ: 17,99\r\n\r\nOK\r\n"},
  {"AT+CREG=1", "\r\nOK\r\n\r\n+CREG: 1\r\n"},
  {"AT+CREG?", "\r\n+CREG: 1,1\r\n\r\nOK\r\n"},
  {"AT+CGATT?", "\r\n+CGATT: 1\r\n\r\nOK\r\n"},
};

/** @brief Response for a command not present in ::k_modem_script. */
static const char k_modem_cme_error[] = "\r\n+CME ERROR: 4\r\n";

/**
 * @struct modem_model_t
 * @brief The modelled modem's whole state.
 */
typedef struct {
  bool     attached;             /**< --modem armed the modem.      */
  char     cmd[k_modem_cmd_cap]; /**< Command-line accumulator.     */
  uint32_t cmd_len;              /**< Bytes buffered in @c cmd.     */
  uint32_t answered;             /**< Commands answered OK/URC.     */
  uint32_t errors;               /**< Commands answered +CME ERROR. */
} modem_model_t;

/** @brief The single modelled modem. */
static modem_model_t s_modem;

/**
 * @brief Copy a NUL-terminated response into @p out, bounded by @p out_cap.
 *
 * @param[in]  resp    NUL-terminated response string.
 * @param[out] out     Destination buffer.
 * @param[in]  out_cap Capacity of @p out in bytes.
 * @return Number of bytes copied (<= @p out_cap).
 */
static uint32_t modem_emit(const char* resp, uint8_t* out, uint32_t out_cap)
{
  uint32_t n = 0U;
  while ((n < out_cap) && (resp[n] != '\0')) {
    out[n] = (uint8_t)resp[n];
    n++;
  }
  return n;
}

/**
 * @brief Answer the completed command line in @c s_modem.cmd.
 *
 * @param[out] out     Destination for the response bytes.
 * @param[in]  out_cap Capacity of @p out in bytes.
 * @return Number of response bytes written.
 */
static uint32_t modem_answer_line(uint8_t* out, uint32_t out_cap)
{
  s_modem.cmd[s_modem.cmd_len] = '\0';
  const uint32_t count         = (uint32_t)(sizeof(k_modem_script) / sizeof(k_modem_script[0]));
  for (uint32_t i = 0U; i < count; i++) {
    if (strcmp(s_modem.cmd, k_modem_script[i].cmd) == 0) {
      s_modem.answered++;
      return modem_emit(k_modem_script[i].resp, out, out_cap);
    }
  }
  s_modem.errors++;
  return modem_emit(k_modem_cme_error, out, out_cap);
}

bool board_modem_attach(void)
{
  s_modem.attached = true;
  board_modem_reset();
  return true;
}

bool board_modem_attached(void)
{
  return s_modem.attached;
}

uint8_t board_modem_channel(void)
{
  return (uint8_t)k_modem_channel;
}

uint32_t board_modem_feed_tx(uint8_t tx, uint8_t* out, uint32_t out_cap)
{
  if ((out == nullptr) || (out_cap == 0U)) {
    return 0U;
  }
  if (tx == (uint8_t)k_modem_lf) {
    return 0U; /* modems terminate on CR; LF (if any) is not a delimiter */
  }
  if (tx == (uint8_t)k_modem_cr) {
    const uint32_t n = modem_answer_line(out, out_cap);
    s_modem.cmd_len  = 0U;
    return n;
  }
  if (s_modem.cmd_len < (uint32_t)(k_modem_cmd_cap - 1U)) {
    s_modem.cmd[s_modem.cmd_len] = (char)tx;
    s_modem.cmd_len++;
  }
  return 0U;
}

void board_modem_reset(void)
{
  s_modem.cmd_len = 0U;
  s_modem.cmd[0]  = '\0';
}

void board_modem_report(void)
{
  if (!s_modem.attached) {
    return;
  }
  if ((s_modem.answered == 0U) && (s_modem.errors == 0U)) {
    return;
  }
  (void)fprintf(stderr,
                "  AT modem      : %u command(s) answered, %u +CME ERROR\n",
                s_modem.answered,
                s_modem.errors);
}
