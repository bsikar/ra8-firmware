/**
 * @file uart.c
 * @brief SCI-based UART polling driver for bring-up debug
 *
 * @details
 * Minimal polling UART driver on top of the SCI peripheral register
 * layer. Intended as a backup log sink for boards without a J-Link
 * (ITM won't drain) or for early-boot messages before ITM is live.
 *
 * Uses standard 8-N-1 framing. Baud rate calculation uses the formula
 * from HUM section 28.2.8:
 *
 * @f[
 *     BRR = (PCLKB / (64 \cdot 2^{(2n-1)} \cdot B)) - 1
 * @f]
 *
 * where `n` is SMR.CKS[1:0] and `B` is the target baud. For a typical
 * bring-up baud of 115200 @ PCLKB = 60 MHz with n = 0:
 *
 *     BRR = (60_000_000 / (64 * 0.5 * 115200)) - 1 = 15.28  -> 15
 *
 * The driver here assumes the caller has already configured pin
 * routing (PFS PSEL = 0b00100 for most SCI variants) and powered
 * the SCI module up via MSTP.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8d2_sci_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"
#include "ra_uart.h"

static const char* s_tag = "UART";

/**
 * @enum ra_uart_init_val_t
 * @brief Magic register values used during SCI UART init.
 */
typedef enum : uint8_t {
  /* NOLINTNEXTLINE(readability-magic-numbers) -- HUM-specified default. */
  k_ra_uart_scmr_default = 0xF2U, /**< Default per HUM; SINV=0, SMIF=0. */
} ra_uart_init_val_t;

/**
 * @enum ra_uart_channel_count_t
 * @brief Total number of SCI channels exposed by the RA8D2.
 */
typedef enum : uint8_t {
  k_ra_uart_channel_count = 10U,
} ra_uart_channel_count_t;

/**
 * @var s_sci_mstp_table
 * @brief Channel-index -> MSTP id lookup.
 *
 * @details
 * One entry per SCI channel. Used by ``ra_uart_init`` to feed the
 * ref-counted ``ra_mstp_enable`` call site.
 */
static const ra_mstp_t s_sci_mstp_table[k_ra_uart_channel_count] = {
  /* HUM Ch 11.2.7 "MSTPCRB : Module Stop Control Register B", p 445 */
  k_ra_mstp_sci0,
  k_ra_mstp_sci1,
  k_ra_mstp_sci2,
  k_ra_mstp_sci3,
  k_ra_mstp_sci4,
  k_ra_mstp_sci5,
  k_ra_mstp_sci6,
  k_ra_mstp_sci7,
  k_ra_mstp_sci8,
  k_ra_mstp_sci9,
};

[[nodiscard]] ra_err_t ra_uart_init(uint8_t channel, uint8_t brr)
{
  volatile r_sci_regs_t* sci = ra_sci(channel);
  RA_CHECK_NULL_PTR(sci, s_tag, "channel out of range");
  if (channel >= k_ra_uart_channel_count) {
    return k_ra_err_invalid_arg;
  }

  const ra_err_t mst_err = ra_mstp_enable(s_sci_mstp_table[channel]);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  /* Disable TX/RX while reprogramming. */
  sci->SCR  = 0U;
  sci->SMR  = 0U; /* 8-N-1, CKS = PCLKB. */
  sci->SCMR = k_ra_uart_scmr_default;
  sci->BRR  = brr;
  sci->SEMR = 0U;

  /* Enable TE + RE. */
  sci->SCR = (uint8_t)((1U << k_ra_scr_bit_te) | (1U << k_ra_scr_bit_re));

  ra_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_uart_putc(uint8_t channel, uint8_t byte)
{
  volatile r_sci_regs_t* sci = ra_sci(channel);
  RA_CHECK_NULL_PTR(sci, s_tag, "channel out of range");

  /* Wait for TDRE (TX data register empty). */
  enum : uint32_t { k_ra_uart_spin_limit = 1000000U };
  for (uint32_t i = 0U; i < k_ra_uart_spin_limit; i++) {
    if ((sci->SSR & (uint8_t)(1U << k_ra_ssr_bit_tdre)) != 0U) {
      sci->TDR = byte;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}
