/**
 * @file uart.c
 * @brief SCI_B-based UART polling driver for bring-up debug
 *
 * @details
 * Minimal polling UART driver on top of the SCI_B peripheral register
 * layer. Intended as a backup log sink for boards without a J-Link
 * (ITM won't drain) or for early-boot messages before ITM is live.
 *
 * Uses standard 8-N-1 framing. Baud rate calculation uses the formula
 * from HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189
 * Table 38.7 with CKS = 0 and the 16x base clock:
 *
 * @f[
 *     BRR = (PCLKB / (32 \cdot B)) - 1
 * @f]
 *
 * For a typical bring-up baud of 115200 @ PCLKB = 60 MHz:
 *
 *     BRR = (60_000_000 / (32 * 115200)) - 1 = 15.28 -> 15
 *
 * The driver here assumes the caller has already configured pin
 * routing and powered the SCI module up via MSTP.
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
 * @enum ra_uart_channel_count_t
 * @brief Total number of SCI channels exposed by the RA8D2.
 */
typedef enum : uint8_t {
  k_ra_uart_channel_count = 10U,
} ra_uart_channel_count_t;

/**
 * @var s_sci_mstp_table
 * @brief Channel-index -> MSTP id lookup.
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

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- disable
   * TX/RX while reprogramming the rest of the channel. */
  sci->CCR0 = 0U;

  /* HUM Ch 38.2.6 "CCR1 : Common Control Register 1", p 2185 -- 8-N-1
   * means parity off, no inversion. */
  sci->CCR1 = 0U;

  /* HUM Ch 38.2.8 "CCR3 : Common Control Register 3", p 2203 -- async
   * mode, 8-bit data (CHR = 10b), 1 stop bit, no FIFO. */
  sci->CCR3 = (k_ra_sci_ccr3_chr_8bit << k_ra_sci_ccr3_shift_chr);

  /* HUM Ch 38.2.7 "CCR2 : Common Control Register 2", p 2189 -- BRR in
   * bits [15:8], MDDR (modulation duty) parked at the reset value
   * so bit-rate modulation stays disabled. */
  sci->CCR2 = ((uint32_t)brr << k_ra_sci_ccr2_shift_brr) |
              (k_ra_sci_mddr_default << k_ra_sci_ccr2_shift_mddr);

  /* HUM Ch 38.2.9 "CCR4 : Common Control Register 4", p 2210 */
  sci->CCR4 = 0U;

  /* HUM Ch 38.2.11 "FCR : FIFO Control Register", p 2215 */
  sci->FCR = 0U;

  /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 -- enable
   * TE + RE. */
  sci->CCR0 = (1U << k_ra_sci_ccr0_bit_te) | (1U << k_ra_sci_ccr0_bit_re);

  ra_log_info_val(s_tag, "init channel", (uint32_t)channel);
  return k_ra_ok;
}

[[nodiscard]] ra_err_t ra_uart_putc(uint8_t channel, uint8_t byte)
{
  volatile r_sci_regs_t* sci = ra_sci(channel);
  RA_CHECK_NULL_PTR(sci, s_tag, "channel out of range");

  /* HUM Ch 38.2.17 "CSR : Common Status Register", p 2225 -- spin
   * until TDRE = 1 (transmit data register empty). */
  enum : uint32_t { k_ra_uart_spin_limit = 1000000U };
  const uint32_t tdre = (1U << k_ra_sci_csr_bit_tdre);
  for (uint32_t i = 0U; i < k_ra_uart_spin_limit; i++) {
    if ((sci->CSR & tdre) != 0U) {
      /* HUM Ch 38.2.3 "TDR : Transmit Data Register", p 2181 */
      sci->TDR = (uint32_t)byte;
      return k_ra_ok;
    }
  }
  return k_ra_err_hw_timeout;
}
