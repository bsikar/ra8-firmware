/**
 * @file ra8_sci_dma_isr.c
 * @brief SCI_B DMA TX/RX descriptors + TXI/RXI/ERI ISR dispatch
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Companion translation unit to ``ra8_sci.c`` for the RA8D2 SCI_B driver
 * (HUM Ch 38). Carries the DMA request-descriptor builders that wire a
 * byte stream to/from TDR/RDR (``ra8_sci_write_dma`` /
 * ``ra8_sci_read_dma``) and the interrupt dispatch entry points
 * (``ra8_sci_dispatch_txi`` / ``ra8_sci_dispatch_rxi`` /
 * ``ra8_sci_dispatch_eri``) that advance the per-channel async byte
 * stream and fire the legacy attach callbacks.
 *
 * The shared per-channel dispatch table (``s_sci_state``) is defined in
 * ``ra8_sci.c`` and reached here through ``ra8_sci_internal.h``. See
 * ``ra8_sci.h`` for the public API contract and ``ra8_sci_regs.h`` for
 * the register layout.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_dma.h"
#include "ra8_dmac.h"
#include "ra8_err.h"
#include "ra8_sci.h"
#include "ra8_sci_internal.h"
#include "ra8_sci_regs.h"

static const char* s_tag = "SCI";

/* ---- DMA TX / RX ----------------------------------------- */

/* Build a DMA request descriptor for byte-stream to/from SCI TDR/RDR -- see surrounding code and HUM citations. */
RA8_INTERNAL static ra8_dma_request_t internal_make_dma_request(uintptr_t             src,
                                                                uintptr_t             dst,
                                                                uint16_t              len,
                                                                bool                  src_inc,
                                                                bool                  dst_inc,
                                                                ra8_dma_complete_fn_t on_complete,
                                                                void*                 ctx)
{
  ra8_dma_request_t req = {};
  req.src_addr          = src;
  req.dst_addr          = dst;
  req.count             = len;
  req.width             = k_ra8_dmac_width_byte;
  req.src_inc           = src_inc;
  req.dst_inc           = dst_inc;
  /* HUM Ch 19 "Event Link Controller (ELC)" p 817 -- trigger routing is a
   * task; until then use software-start and drive the first
   * element from the polling path or rely on ra8_fake_dma for host tests. */
  req.trigger     = (ra8_elc_event_t)0;
  req.on_complete = on_complete;
  req.ctx         = ctx;
  return req;
}

ra8_err_t ra8_sci_write_dma(uint8_t               channel,
                            const uint8_t*        data,
                            uint16_t              len,
                            ra8_dma_complete_fn_t on_complete,
                            void*                 ctx,
                            uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(data, s_tag, "write_dma: data");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "write_dma: out_dma_channel");
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if ((reg == nullptr) || (len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.3 "TDR : Transmit Data Register", p 2181 -- DMA writes
   * land in TDR; the engine streams one byte per element while the
   * source increments across data[]. */
  const ra8_dma_request_t req = internal_make_dma_request((uintptr_t)data,
                                                          (uintptr_t)&reg->TDR,
                                                          len,
                                                          /*src_inc=*/true,
                                                          /*dst_inc=*/false,
                                                          on_complete,
                                                          ctx);
  return ra8_dma_request(&req, out_dma_channel);
}

/* out_buf is written by the DMAC engine via the dst_addr path, not
 * through the pointer directly, so clang-tidy would otherwise flag
 * it as a const candidate. */
ra8_err_t ra8_sci_read_dma(
  uint8_t channel,
  uint8_t*
    out_buf, // NOLINT(readability-non-const-parameter) -- written by the DMAC engine via dst_addr, never through the pointer.
  uint16_t              len,
  ra8_dma_complete_fn_t on_complete,
  void*                 ctx,
  uint8_t*              out_dma_channel)
{
  RA8_CHECK_NULL_PTR(out_buf, s_tag, "read_dma: out_buf");
  RA8_CHECK_NULL_PTR(out_dma_channel, s_tag, "read_dma: out_dma_channel");
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if ((reg == nullptr) || (len == 0U)) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 38.2.2 "RDR : Receive Data Register", p 2180 -- RDR is
   * read-once per element; destination increments across out_buf[]. */
  const ra8_dma_request_t req = internal_make_dma_request((uintptr_t)&reg->RDR,
                                                          (uintptr_t)out_buf,
                                                          len,
                                                          /*src_inc=*/false,
                                                          /*dst_inc=*/true,
                                                          on_complete,
                                                          ctx);
  return ra8_dma_request(&req, out_dma_channel);
}

/* ---- ISR dispatch ----------------------------------------------------- */

RA8_ISR_SAFE
void ra8_sci_dispatch_txi(uint8_t channel)
{
  if (channel > k_ra8_sci_channel_max_index) {
    return;
  }
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return;
  }
  const ra8_sci_tx_fn_t cb  = s_sci_state[channel].tx_fn;
  void* const           ctx = s_sci_state[channel].tx_ctx;
  const uint32_t        tie = (1U << k_ra8_sci_ccr0_bit_tie);

  /* Async byte-stream path (ra8_sci_write). Mirrors FSP r_sci_b_uart
   * `txi_isr` (r_sci_b_uart.c) which decrements `tx_src_bytes`
   * each TXI and clears TIE when the count hits zero. */
  if (s_sci_state[channel].tx_len > 0U) {
    if (s_sci_state[channel].tx_idx < s_sci_state[channel].tx_len) {
      const uint8_t byte = s_sci_state[channel].tx_buf[s_sci_state[channel].tx_idx];
      s_sci_state[channel].tx_idx += 1U;
      /* HUM Ch 38.2.3 "TDR : Transmit Data Register", p 2181 */
      reg->TDR = (uint32_t)byte;
      /* Fire user-attached TX visibility callback per byte; its
       * boolean return is ignored here because the async path owns
       * the byte-stream now. */
      if (cb != nullptr) {
        uint8_t echo = byte;
        (void)cb(ctx, &echo);
      }
    }
    if (s_sci_state[channel].tx_idx >= s_sci_state[channel].tx_len) {
      /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
      reg->CCR0                   = reg->CCR0 & ~tie;
      s_sci_state[channel].tx_buf = nullptr;
      s_sci_state[channel].tx_len = 0U;
      s_sci_state[channel].tx_idx = 0U;
    }
    return;
  }

  /* Legacy callback-only path (ra8_sci_attach_tx_handler). */
  if (cb == nullptr) {
    /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
    reg->CCR0 = reg->CCR0 & ~tie;
    return;
  }
  uint8_t byte = 0U;
  if (cb(ctx, &byte)) {
    /* HUM Ch 38.2.3 "TDR : Transmit Data Register", p 2181 */
    reg->TDR = (uint32_t)byte;
  } else {
    reg->CCR0 = reg->CCR0 & ~tie;
  }
}

RA8_ISR_SAFE
void ra8_sci_dispatch_rxi(uint8_t channel)
{
  if (channel > k_ra8_sci_channel_max_index) {
    return;
  }
  volatile r_sci_regs_t* reg = ra8_sci(channel);
  if (reg == nullptr) { /* GCOVR_EXCL_BR_LINE -- bounds already validated */
    return;
  }
  const ra8_sci_rx_fn_t cb  = s_sci_state[channel].rx_fn;
  void* const           ctx = s_sci_state[channel].rx_ctx;
  /* HUM Ch 38.2.2 "RDR : Receive Data Register", p 2180 */
  const uint8_t b = (uint8_t)(reg->RDR & k_ra8_sci_rdr_mask_data8);

  /* Async byte-stream path (ra8_sci_read). Mirrors FSP r_sci_b_uart
   * `rxi_isr` which appends each byte into `p_rx_dest` until
   * `rx_dest_bytes` is satisfied, then signals UART_EVENT_RX_COMPLETE. */
  if (s_sci_state[channel].rx_len > 0U) {
    if (s_sci_state[channel].rx_idx < s_sci_state[channel].rx_len) {
      s_sci_state[channel].rx_buf[s_sci_state[channel].rx_idx] = b;
      s_sci_state[channel].rx_idx += 1U;
    }
    if (s_sci_state[channel].rx_idx >= s_sci_state[channel].rx_len) {
      const uint32_t rie = (1U << k_ra8_sci_ccr0_bit_rie);
      /* HUM Ch 38.2.5 "CCR0 : Common Control Register 0", p 2182 */
      reg->CCR0                   = reg->CCR0 & ~rie;
      s_sci_state[channel].rx_buf = nullptr;
      s_sci_state[channel].rx_len = 0U;
      s_sci_state[channel].rx_idx = 0U;
    }
  }

  /* Legacy attach path: still fires per byte even when an async RX is
   * in flight, so existing flow-control hooks keep working. */
  if (cb != nullptr) {
    cb(ctx, b);
  }
}

RA8_ISR_SAFE
void ra8_sci_dispatch_eri(uint8_t channel)
{
  if (channel > k_ra8_sci_channel_max_index) {
    return;
  }
  (void)ra8_sci_clear_errors(channel);
}
