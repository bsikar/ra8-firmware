/**
 * @file board_periph_sci.c
 * @brief SCI_B UART peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8D2 SCI_B serial channels (ra8_sci_regs.h, the 32-bit-register
 * variant) with enough fidelity for the console examples to run on the emulator
 * (ra8_sci.c semantics):
 *
 *  - **TX** is always "drained" -- CSR.TDRE / CSR.TEND read as set so the polled
 *    transmit-empty / transmit-end waits fall through. Each byte the firmware
 *    writes to TDR is counted, captured into a last-line buffer (latched on
 *    newline so the board view can show the last console line), and handed to a
 *    host sink (main.c prints it as @c [uart] SCIn: ...).
 *  - **RX** is a per-channel ring the host fills via ::board_periph_sci_feed_rx;
 *    CSR.RDRF (and the FIFO RX flags) read set while bytes are queued, RDR reads
 *    return them oldest-first.
 *  - **Interrupts**: on the console channel an enabled TXI / TEI re-pends each
 *    tick (so an interrupt-driven transmitter keeps streaming with TX always
 *    drained) and an enabled RXI pends while queued RX bytes remain -- all
 *    through the core's ICU -> NVIC path.
 *
 * Self-registers its descriptor (address range + read / write / tick / reset /
 * report) with the board_periph core from a file-scope constructor.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph.h"
#include "board_periph_block.h"
#include "board_periph_modem.h"
#include "board_periph_sd.h"

/** @brief SCI_B block geometry (ra8_sci_regs.h, 32-bit-register variant). */
typedef enum : uint64_t {
  k_sci_base      = 0x40358000UL,   /**< SCI0 base (Secure alias).      */
  k_sci_stride    = 0x100UL,        /**< Bytes per SCI channel.         */
  k_sci_count     = 10UL,           /**< SCI0..SCI9.                    */
  k_sci_span      = 0x100UL * 10UL, /**< SCI span.                      */
  k_sci_off_rdr   = 0x00UL,         /**< RDR receive data (RDAT[7:0]).  */
  k_sci_off_tdr   = 0x04UL,         /**< TDR transmit data (TDAT[7:0]). */
  k_sci_off_ccr0  = 0x08UL,         /**< CCR0 (TE/RE/RIE/TIE/TEIE).     */
  k_sci_off_csr   = 0x48UL,         /**< CSR (TDRE/TEND/RDRF + errors). */
  k_sci_off_frsr  = 0x50UL,         /**< FRSR FIFO receive status.      */
  k_sci_off_ftsr  = 0x54UL,         /**< FTSR FIFO transmit status.     */
  k_sci_off_cfclr = 0x68UL,         /**< CFCLR common flag clear (W1C). */
  k_sci_off_ffclr = 0x70UL,         /**< FFCLR FIFO flag clear (W1C).   */
} sci_map_t;

/** @brief SCI_B CCR0 interrupt/enable bits (ra8_sci_ccr0_bit_t). */
typedef enum : uint32_t {
  k_sci_ccr0_re   = 0x00000001U, /**< RE  receive enable (bit 0).         */
  k_sci_ccr0_te   = 0x00000010U, /**< TE  transmit enable (bit 4).        */
  k_sci_ccr0_rie  = 0x00010000U, /**< RIE receive-interrupt enable (16).  */
  k_sci_ccr0_tie  = 0x00100000U, /**< TIE transmit-interrupt enable (20). */
  k_sci_ccr0_teie = 0x00200000U, /**< TEIE transmit-end int enable (21).  */
} sci_ccr0_bit_t;

/** @brief SCI_B CSR status bits (ra8_sci_csr_bit_t). */
typedef enum : uint32_t {
  k_sci_csr_rxdmon = 0x00008000U, /**< RXDMON RXD pin monitor (bit 15).   */
  k_sci_csr_tdre   = 0x20000000U, /**< TDRE transmit-data-empty (bit 29). */
  k_sci_csr_tend   = 0x40000000U, /**< TEND transmit-end (bit 30).        */
  k_sci_csr_rdrf   = 0x80000000U, /**< RDRF receive-data-full (bit 31).   */
} sci_csr_bit_t;

/** @brief SCI_B FIFO status bits used by reads of FRSR / FTSR. */
typedef enum : uint32_t {
  k_sci_frsr_dr   = 0x00000001U, /**< FRSR.DR receive-data-ready (bit 0).  */
  k_sci_frsr_rdf  = 0x00000040U, /**< FRSR.RDF receive-FIFO-data-full (6). */
  k_sci_ftsr_tdfe = 0x00000040U, /**< FTSR.TDFE transmit-FIFO-empty (6).   */
} sci_fifo_bit_t;

/** @brief CFCLR / FFCLR write-1-to-clear bits this model honours. */
typedef enum : uint32_t {
  k_sci_cfclr_rdrfc = 0x80000000U, /**< RDRFC clear CSR.RDRF (bit 31). */
  k_sci_ffclr_drc   = 0x00000001U, /**< DRC clear FRSR.DR (bit 0).     */
} sci_clr_bit_t;

/**
 * @brief SCI8 ELC event numbers the console channel raises (FSP ra8d2 bsp_elc).
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19) lays SCI channel events out in
 * RXI / TXI / TEI / ERI / AM order; for the EK-RA8D2 console channel (SCI8,
 * PD02/PD03) these are RXI 0x122, TXI 0x123, TEI 0x124, matching FSP
 * `bsp_elc.h` for ra8d2 (`ELC_EVENT_SCI8_RXI = 0x122`, `_TXI = 0x123`,
 * `_TEI = 0x124`). A firmware that routes one through ra8_isr_register writes the
 * same number into an IELSR slot, so the ICU model matches the raised event to
 * that slot exactly as it does for the timer events.
 */
typedef enum : uint16_t {
  k_event_sci8_rxi = 0x122U, /**< SCI8 RXI receive-data-full event.   */
  k_event_sci8_txi = 0x123U, /**< SCI8 TXI transmit-data-empty event. */
  k_event_sci8_tei = 0x124U, /**< SCI8 TEI transmit-end event.        */
} sci_elc_event_t;

/** @brief SCI_B model sizing and the EK-RA8D2 console channel. */
typedef enum : uint32_t {
  k_sci_console_ch     = 8U,    /**< EK-RA8D2 console = SCI8 (PD02/PD03).        */
  k_sci_sd_ch          = 0U,    /**< EK-RA8D2 Pmod2 microSD = SCI0 Simple-SPI.   */
  k_sci_rx_queue_len   = 512U,  /**< Per-channel host->firmware RX capacity.     */
  k_sci_data_mask      = 0xFFU, /**< RDR/TDR data field is 8 bits.               */
  k_uart_line_cap      = 256U,  /**< Captured last-TX-line buffer capacity.      */
  k_sci_spi_cipo_idle  = 0xFFU, /**< CIPO idles high (0xFF) on an empty SPI bus. */
  k_sci_modem_resp_cap = 96U,   /**< Scratch for one AT-modem reply burst.       */
} sci_tune_t;

/**
 * @brief One SCI_B channel: control shadow + a host-fed RX byte queue.
 *
 * @details
 * TX is always "drained" in the model (TDRE/TEND read as set), so only the
 * control shadow (@c ccr0) and a transmitted-byte counter are kept on that
 * side. RX is a simple ring of bytes the host queued via
 * ::board_periph_sci_feed_rx; @c rx_head / @c rx_tail bound the live span and
 * @c received counts every byte the firmware has consumed.
 */
typedef struct {
  uint32_t ccr0;                   /**< CCR0 shadow (TE/RE/RIE/TIE/TEIE). */
  uint32_t transmitted;            /**< Bytes captured from TDR writes.   */
  uint32_t received;               /**< Bytes the firmware read from RDR. */
  uint8_t  rx[k_sci_rx_queue_len]; /**< Host->firmware byte ring.         */
  uint32_t rx_head;                /**< Next byte the firmware will read. */
  uint32_t rx_tail;                /**< Next free slot for a queued byte. */
  uint32_t rx_dropped;             /**< Bytes dropped on a full RX ring.  */
} sci_state_t;

static sci_state_t s_sci[k_sci_count];

/** @brief Host sink for transmitted bytes (main.c installs the stdout sink). */
static void (*s_sci_tx_sink)(uint8_t channel, uint8_t byte);

/* Last complete console line, latched on newline so the board view can show
 * what a non-display example printed (e.g. "hello, ra8d2!"). s_uart_pend
 * accumulates the in-flight line; s_uart_last holds the last finished one. The
 * deeper scrollback ring now lives in board_console (the UART channel), which
 * the board view's tabbed console reads back -- this model just latches the last
 * line and routes each completed line there. */
static char     s_uart_last[k_uart_line_cap]; /**< Last completed TX line.        */
static char     s_uart_pend[k_uart_line_cap]; /**< Line being accumulated.        */
static uint32_t s_uart_pend_len;              /**< Chars buffered in s_uart_pend. */

void board_periph_sci_set_tx_sink(void (*sink)(uint8_t channel, uint8_t byte))
{
  s_sci_tx_sink = sink;
}

uint8_t board_periph_sci_console_channel(void)
{
  return (uint8_t)k_sci_console_ch;
}

void board_periph_sci_feed_rx(uint8_t channel, const uint8_t* data, uint32_t len)
{
  if ((channel >= (uint32_t)k_sci_count) || (data == nullptr)) {
    return;
  }
  sci_state_t* s = &s_sci[channel];
  for (uint32_t i = 0U; i < len; i++) {
    const uint32_t next = (s->rx_tail + 1U) % (uint32_t)k_sci_rx_queue_len;
    if (next == s->rx_head) {
      s->rx_dropped += (len - i); /* ring full: drop the remaining bytes */
      if (board_periph_trace()) {
        (void)
          fprintf(stderr, "  [trace] SCI%u RX queue full, dropped %u bytes\n", channel, len - i);
      }
      return;
    }
    s->rx[s->rx_tail] = data[i];
    s->rx_tail        = next;
  }
}

const char* board_periph_uart_last_line(void)
{
  return s_uart_last;
}

uint32_t board_periph_uart_tx_total(void)
{
  uint32_t total = 0U;
  for (uint32_t ch = 0U; ch < (uint32_t)k_sci_count; ch++) {
    total += s_sci[ch].transmitted;
  }
  return total;
}

/** @brief True iff the channel has a queued, unread host RX byte. */
static bool sci_rx_available(const sci_state_t* s)
{
  return s->rx_head != s->rx_tail;
}

/** @brief Build the CSR value: TX always drained, RDRF reflects the RX queue. */
static uint32_t sci_csr_value(const sci_state_t* s)
{
  /* TDRE + TEND are held set so ra8_sci's "wait for transmit empty / end" polls
   * (ra8_sci_putc_polling on TDRE, internal_wait_tx_end on TEND) fall through.
   * RXDMON reads high because an idle UART line idles high. RDRF tracks the
   * host RX queue so ra8_sci_getc_polling completes only when a byte is ready. */
  uint32_t csr = (uint32_t)k_sci_csr_tdre | (uint32_t)k_sci_csr_tend | (uint32_t)k_sci_csr_rxdmon;
  if (sci_rx_available(s)) {
    csr |= (uint32_t)k_sci_csr_rdrf;
  }
  return csr;
}

/** @brief Dispatch an SCI read for channel @p ch at byte offset @p off. */
static uint64_t sci_reg_read(uint32_t ch, uint64_t off)
{
  sci_state_t* s = &s_sci[ch];
  if (off == (uint64_t)k_sci_off_rdr) {
    if (!sci_rx_available(s)) {
      return 0U; /* drained: nothing queued */
    }
    const uint8_t b = s->rx[s->rx_head];
    s->rx_head      = (s->rx_head + 1U) % (uint32_t)k_sci_rx_queue_len;
    s->received++;
    return (uint64_t)b;
  }
  if (off == (uint64_t)k_sci_off_csr) {
    return sci_csr_value(s);
  }
  if (off == (uint64_t)k_sci_off_ccr0) {
    return s->ccr0;
  }
  if (off == (uint64_t)k_sci_off_frsr) {
    return sci_rx_available(s) ? ((uint32_t)k_sci_frsr_dr | (uint32_t)k_sci_frsr_rdf) : 0U;
  }
  if (off == (uint64_t)k_sci_off_ftsr) {
    return (uint32_t)k_sci_ftsr_tdfe; /* TX FIFO is always empty in the model */
  }
  return 0U; /* CFCLR / FFCLR read as 0; other regs unmodelled -> 0 */
}

/**
 * @brief Accumulate one transmitted byte into the captured last-line buffer.
 *
 * @details
 * Mirrors the host console formatter: a newline latches the pending line into
 * ::s_uart_last (so the board view can show the last complete console line),
 * CR is dropped, and any other byte is appended until the buffer is full. This
 * tracks only the last completed line, independent of the optional TX sink.
 *
 * @param[in] byte The transmitted data byte.
 * @return Nothing.
 * @since 0.1.0
 */
static void sci_capture_tx_line(uint8_t byte)
{
  if (byte == (uint8_t)'\n') {
    for (uint32_t i = 0U; i < s_uart_pend_len; i++) {
      s_uart_last[i] = s_uart_pend[i];
    }
    s_uart_last[s_uart_pend_len] = '\0';
    /* Route the completed line into the board_console UART channel (which backs
     * the tabbed board-view console + its deeper scrollback). */
    board_console_push(k_board_console_ch_uart, s_uart_last);
    s_uart_pend_len = 0U;
    return;
  }
  if ((byte != (uint8_t)'\r') && (s_uart_pend_len < (uint32_t)(k_uart_line_cap - 1U))) {
    s_uart_pend[s_uart_pend_len++] = (char)byte;
  }
}

/** @brief Dispatch an SCI write; TDR is captured, CCR0 shadowed, clears no-op. */
static void sci_reg_write(uint32_t ch, uint64_t off, uint32_t value)
{
  sci_state_t* s = &s_sci[ch];
  if (off == (uint64_t)k_sci_off_tdr) {
    /* Both the polled (ra8_sci_putc_polling) and interrupt (ra8_sci_dispatch_txi)
     * paths launch a frame by writing TDAT[7:0]; FIFO mode also writes TDR. */
    const uint8_t byte = (uint8_t)(value & (uint32_t)k_sci_data_mask);
    s->transmitted++;
    /* Only the console channel carries UART text. Other channels (e.g. SCI0 in
     * Simple-SPI mode for the microSD) move binary frames, so capturing + sinking
     * their TX would spew the SPI traffic as "[uart] SCIn:" garbage -- skip it. */
    if (ch == (uint32_t)k_sci_console_ch) {
      sci_capture_tx_line(byte);
      if (s_sci_tx_sink != nullptr) {
        s_sci_tx_sink((uint8_t)ch, byte);
      }
    }
    /* Pmod2 microSD is SCI0 in Simple-SPI mode: every TDR write clocks a
     * full-duplex frame, so feed the modelled card's response back into this
     * channel's RX queue (RDR/RDRF) -- the real ra8_sci_spi + ra8_sdmmc_spi path
     * then runs against the FAT image exactly as on hardware. */
    if (ch == (uint32_t)k_sci_sd_ch) {
      /* With a card attached, return its modelled response; with none, return
       * 0xFF (CIPO idles high on an empty bus). Always feeding a byte is what
       * keeps RDRF advancing -- otherwise the firmware's full-duplex read polls
       * RDRF forever and every ra8_sdmmc probe burns its whole timeout budget
       * (seconds, per byte), which is the dominant cold-boot stall for any app
       * that probes the SD without a card present. With 0xFF the probe simply
       * sees no valid R1 and fails "no card" promptly, as on real hardware. */
      const uint8_t resp =
        board_sd_attached() ? board_sd_exchange(byte) : (uint8_t)k_sci_spi_cipo_idle;
      board_periph_sci_feed_rx((uint8_t)ch, &resp, 1U);
    }
    /* MikroBUS UART (SCI7) with a modelled AT modem attached: forward each
     * transmitted byte to the modem line model. Most bytes buffer silently; the
     * terminating CR closes an AT command and the model returns the modem's
     * reply, which we feed back into this channel's RX queue so the firmware's
     * genuine ra8_modem_at -> ra8_sci_getc_polling path drains it (EIL == HIL). */
    if ((ch == (uint32_t)board_modem_channel()) && board_modem_attached()) {
      uint8_t        mresp[k_sci_modem_resp_cap] = {};
      const uint32_t mn = board_modem_feed_tx(byte, mresp, (uint32_t)k_sci_modem_resp_cap);
      if (mn > 0U) {
        board_periph_sci_feed_rx((uint8_t)ch, mresp, mn);
      }
    }
  } else if (off == (uint64_t)k_sci_off_ccr0) {
    s->ccr0 = value;
  }
  /* CFCLR / FFCLR are write-1-to-clear: in this model TDRE/TEND stay asserted
   * and RDRF is derived from the live RX queue, so clearing them is a no-op
   * (the firmware re-reads the queue-backed state on its next poll). */
}

/** @brief Raise this channel's enabled SCI interrupts through the ICU. */
static void sci_tick_channel(uc_engine* uc, uint32_t ch)
{
  if (ch != (uint32_t)k_sci_console_ch) {
    return; /* only the console channel has modelled ELC event numbers */
  }
  const sci_state_t* s = &s_sci[ch];
  /* TX is always empty/ended in the model: while the firmware keeps TIE/TEIE
   * armed (an interrupt-driven ra8_sci_write in flight), re-pend TXI/TEI so the
   * dispatcher pushes the next byte exactly as a real TDRE/TEND would. */
  if ((s->ccr0 & (uint32_t)k_sci_ccr0_te) != 0U) {
    if ((s->ccr0 & (uint32_t)k_sci_ccr0_tie) != 0U) {
      board_periph_icu_raise_event(uc, (uint16_t)k_event_sci8_txi);
    }
    if ((s->ccr0 & (uint32_t)k_sci_ccr0_teie) != 0U) {
      board_periph_icu_raise_event(uc, (uint16_t)k_event_sci8_tei);
    }
  }
  /* RX: while RIE is armed and a host byte is queued, pend RXI so the firmware
   * reads it from RDR in handler context (interrupt-driven receive). */
  if (((s->ccr0 & (uint32_t)k_sci_ccr0_rie) != 0U) && sci_rx_available(s)) {
    board_periph_icu_raise_event(uc, (uint16_t)k_event_sci8_rxi);
  }
}

/** @brief MMIO read inside the SCI window: route to the addressed channel. */
static uint64_t sci_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_sci_base) / (uint64_t)k_sci_stride);
  return sci_reg_read(ch, (addr - (uint64_t)k_sci_base) % (uint64_t)k_sci_stride);
}

/** @brief MMIO write inside the SCI window: route to the addressed channel. */
static void sci_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_sci_base) / (uint64_t)k_sci_stride);
  sci_reg_write(ch, (addr - (uint64_t)k_sci_base) % (uint64_t)k_sci_stride, (uint32_t)value);
}

/** @brief Service every SCI channel once per emulation chunk. */
static void sci_tick(uc_engine* uc)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_sci_count; ch++) {
    sci_tick_channel(uc, ch);
  }
}

/** @brief Clear all SCI channel state and the captured last-line buffers. */
static void sci_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_sci_count; i++) {
    s_sci[i] = (sci_state_t){};
  }
  s_uart_last[0]  = '\0';
  s_uart_pend[0]  = '\0';
  s_uart_pend_len = 0U;
  /* Re-frame the AT modem's command accumulator on a block reset (--reboot),
   * keeping its attached flag -- the SCI reset re-frames transport, not the
   * modem's presence, mirroring board_periph_eink.c's reset. */
  board_modem_reset();
}

/** @brief Print one line per SCI channel that moved any bytes. */
static void sci_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_sci_count; ch++) {
    if ((s_sci[ch].transmitted > 0U) || (s_sci[ch].received > 0U)) {
      (void)fprintf(stderr,
                    "  SCI%u UART     : TX %u bytes  RX %u bytes  (RX dropped %u)\n",
                    ch,
                    s_sci[ch].transmitted,
                    s_sci[ch].received,
                    s_sci[ch].rx_dropped);
    }
  }
  /* An attached AT modem answers on the modem channel; summarise its traffic
   * alongside the raw SCI byte counts (self-contained -- no main.c edit). */
  board_modem_report();
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_sci_block = {
  .base   = (uint64_t)k_sci_base,
  .span   = (uint64_t)k_sci_span,
  .order  = (uint32_t)k_block_order_sci,
  .read   = sci_read,
  .write  = sci_write,
  .tick   = sci_tick,
  .reset  = sci_reset,
  .report = sci_report,
  .name   = "SCI_B UART",
};

/** @brief Self-register the SCI block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_sci_register(void)
{
  board_periph_register_block(&k_sci_block);
}
