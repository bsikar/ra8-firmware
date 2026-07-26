/**
 * @file board_periph_ssie.c
 * @brief Serial Sound Interface Enhanced (SSIE / I2S) peripheral-block model
 *
 * @details
 * Models the RA8D2 SSIE (ra8_ssie_regs.h, ra8_ssie.c) closely enough that the
 * @c ssie_audio_loop example runs on the emulator instead of bailing to its
 * panic-halt. Two channels (SSIE0 @c 0x4025D000, SSIE1 @c 0x4025D100, 0x100
 * stride) are modelled as a per-channel register shadow with two read overrides
 * that the driver's bring-up actually depends on:
 *
 *  - **SSISR.IIRQ** (idle, bit 25). @c ra8_ssie_start refuses to set REN/TEN
 *    unless it first reads SSISR with IIRQ asserted (otherwise @c k_ra8_err_busy).
 *    The model returns IIRQ set while the channel is idle (REN and TEN both
 *    clear) and clears it once REN/TEN are set, exactly as the real idle flag
 *    behaves, so the start sequence proceeds.
 *  - **SSIFSR.TDE** (transmit FIFO empty, bit 16). Held set so the transmit
 *    path always has room; @c ra8_ssie_write_sample writes SSIFTDR, which the
 *    model drains (counting samples for the end-of-run report) rather than
 *    backing up.
 *
 * Every other register is a faithful write/readback shadow, so a "configure
 * then verify" read returns what was written (SSICR config, SSIFCR FIFO control,
 * SSIRST, SSIOFR audio format). The model does not synthesise audio clocks or
 * the I2S frame -- it reproduces the controller handshake the firmware drives,
 * matching the rest of board_periph (it validates "does the firmware program the
 * SSIE correctly", not silicon timing).
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief Console-tap line buffer capacity for an I2S/SSIE summary. */
typedef enum : uint32_t {
  k_ssie_console_line_cap = 48U, /**< Max chars in an "I2S ch.. tx=.." line. */
} ssie_console_t;

/** @brief SSIE block geometry (ra8_ssie_regs.h). */
typedef enum : uint64_t {
  k_ssie_base   = 0x4025D000UL,  /**< SSIE0 base.                                     */
  k_ssie_stride = 0x100UL,       /**< Bytes per SSIE channel.                         */
  k_ssie_count  = 2UL,           /**< SSIE0, SSIE1.                                   */
  k_ssie_span   = 0x100UL * 2UL, /**< Ssie span.                                      */
  k_ssie_regs   = 0x40UL,        /**< Shadowed 32-bit words per channel (0x00..0xFC). */
} ssie_map_t;

/** @brief Per-channel register byte offsets (ra8_ssie_regs.h). */
typedef enum : uint64_t {
  k_ssie_off_ssicr   = 0x00UL, /**< Control (REN/TEN, MST, DWL, SWL ...). */
  k_ssie_off_ssisr   = 0x04UL, /**< Status (IIRQ + over/underflow flags). */
  k_ssie_off_ssifcr  = 0x10UL, /**< FIFO control (RFRST/TFRST ...).       */
  k_ssie_off_ssifsr  = 0x14UL, /**< FIFO status (RDF, TDE, RDC, TDC).     */
  k_ssie_off_ssiftdr = 0x18UL, /**< Transmit FIFO data.                   */
  k_ssie_off_ssifrdr = 0x1CUL, /**< Receive FIFO data.                    */
} ssie_off_t;

/** @brief SSICR / SSISR / SSIFSR field masks used by the model. */
typedef enum : uint32_t {
  k_ssie_ren_ten = 0x00000003U, /**< SSICR REN | TEN.                    */
  k_ssie_iirq    = 0x02000000U, /**< SSISR IIRQ idle-mode flag (bit 25). */
  k_ssie_tde     = 0x00010000U, /**< SSIFSR TDE transmit-FIFO-empty.     */
} ssie_field_t;

/** @brief Per-tick order slot for the SSIE block (relative order only). */
typedef enum : uint32_t {
  k_ssie_block_order = 130U, /**< After the existing blocks; report order. */
} ssie_order_t;

/** @brief One SSIE channel: a flat register shadow + a TX sample tally. */
typedef struct {
  uint32_t reg[k_ssie_regs]; /**< 32-bit register shadow by word index. */
  uint32_t tx_samples;       /**< SSIFTDR writes drained.               */
} ssie_state_t;

static ssie_state_t s_ssie[k_ssie_count];

/** @brief Reset all SSIE channel shadows to power-on (zeroed) state. */
static void ssie_reset(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_ssie_count; ch++) {
    for (uint32_t i = 0U; i < (uint32_t)k_ssie_regs; i++) {
      s_ssie[ch].reg[i] = 0U;
    }
    s_ssie[ch].tx_samples = 0U;
  }
}

/** @brief MMIO read inside the SSIE window: route to the addressed channel. */
static uint64_t ssie_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_ssie_base) / (uint64_t)k_ssie_stride);
  const uint64_t off = (addr - (uint64_t)k_ssie_base) % (uint64_t)k_ssie_stride;
  if (ch >= (uint32_t)k_ssie_count) {
    return 0U;
  }
  const ssie_state_t* s = &s_ssie[ch];
  if (off == (uint64_t)k_ssie_off_ssisr) {
    /* IIRQ asserts while the channel is idle (REN and TEN both clear), so the
     * driver's "must be idle before enabling" gate in ra8_ssie_start passes. */
    const bool running = (s->reg[k_ssie_off_ssicr / 4U] & (uint32_t)k_ssie_ren_ten) != 0U;
    return running ? 0U : (uint64_t)k_ssie_iirq;
  }
  if (off == (uint64_t)k_ssie_off_ssifsr) {
    return (uint64_t)k_ssie_tde; /* TX FIFO always has room; RX empty. */
  }
  if (off == (uint64_t)k_ssie_off_ssifrdr) {
    return 0U; /* No RX audio source in the model. */
  }
  if ((off % 4UL) != 0UL) {
    return 0U;
  }
  const uint32_t idx = (uint32_t)(off / 4UL);
  return (idx < (uint32_t)k_ssie_regs) ? (uint64_t)s->reg[idx] : 0U;
}

/** @brief MMIO write inside the SSIE window: route to the addressed channel. */
static void ssie_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch  = (uint32_t)((addr - (uint64_t)k_ssie_base) / (uint64_t)k_ssie_stride);
  const uint64_t off = (addr - (uint64_t)k_ssie_base) % (uint64_t)k_ssie_stride;
  if (ch >= (uint32_t)k_ssie_count) {
    return;
  }
  ssie_state_t* s = &s_ssie[ch];
  if (off == (uint64_t)k_ssie_off_ssiftdr) {
    s->tx_samples++; /* Transmit FIFO drains immediately in the model. */
    return;
  }
  if ((off % 4UL) != 0UL) {
    return;
  }
  const uint32_t idx = (uint32_t)(off / 4UL);
  if (idx < (uint32_t)k_ssie_regs) {
    s->reg[idx] = (uint32_t)value;
  }
}

/** @brief End-of-run SSIE section: per-channel enable state + TX tally. */
static void ssie_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_ssie_count; ch++) {
    const uint32_t ssicr = s_ssie[ch].reg[k_ssie_off_ssicr / 4U];
    if ((ssicr == 0U) && (s_ssie[ch].tx_samples == 0U)) {
      continue; /* Untouched channel: stay quiet. */
    }
    /* Console I2S tab: per-channel TX-sample tally at end-of-run. I2S streams a
     * sample per SSIFTDR write (far too fast to log live), so the bounded
     * transfer summary is the whole-run tally -- the same shape spi_report uses. */
    char ln[k_ssie_console_line_cap];
    (void)snprintf(ln,
                   sizeof(ln),
                   "I2S ch%u tx_samples=%u",
                   (unsigned)ch,
                   (unsigned)s_ssie[ch].tx_samples);
    board_console_push(k_board_console_ch_i2s, ln);
    (void)fprintf(stderr,
                  "  SSIE%u         : TEN=%u REN=%u tx_samples=%u\n",
                  ch,
                  (unsigned)((ssicr >> 1) & 1U),
                  (unsigned)(ssicr & 1U),
                  s_ssie[ch].tx_samples);
  }
}

/** @brief SSIE block descriptor (self-registered with the core). */
static const board_periph_block_t k_ssie_block = {
  .base   = (uint32_t)k_ssie_base,
  .span   = (uint32_t)k_ssie_span,
  .order  = (uint32_t)k_ssie_block_order,
  .read   = ssie_read,
  .write  = ssie_write,
  .tick   = nullptr,
  .reset  = ssie_reset,
  .report = ssie_report,
  .name   = "SSIE",
};

/** @brief Register the SSIE block before main (host constructor). */
[[gnu::constructor]] static void ssie_block_register(void)
{
  board_periph_register_block(&k_ssie_block);
}
