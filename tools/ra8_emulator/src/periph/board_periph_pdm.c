/**
 * @file board_periph_pdm.c
 * @brief PDM-IF (digital MEMS microphone) peripheral-block model for ra8_emulator
 *
 * @details
 * Models the single RA8D2 PDM-IF block at 0x4025_6000 (ra8_pdm_regs.h /
 * ra8_pdm.c, the real HUM Ch 49 PDM-IF). The firmware read path
 * (``ra8_pdm_read``) polls a channel's data-status register ``PDDSR.NUM`` for the
 * FIFO fill count, then drains that many 20-bit signed PCM samples out of the
 * data-read register ``PDDRR``. The stop path (``ra8_pdm_stop``) polls the common
 * channel-status register ``PDCSR`` until the channel's run bit clears.
 *
 * This block turns that register set into a synthetic-audio source. A channel
 * only produces samples once it has been BOTH started (a ``PDCSTRTR`` bit for it)
 * AND had its data read enabled (``PDDRCR.DATRE``) -- the honest-model rule from
 * issue #218: an unconfigured / unstarted read yields zero samples (``PDDSR.NUM``
 * reads 0, ``PDDRR`` reads 0), never a faked pass. Once live, ``PDDSR`` reports a
 * full FIFO and each ``PDDRR`` read yields the next sample of a deterministic
 * low-amplitude triangle tone plus a small pseudo-random dither, so the tone is
 * a genuine non-degenerate signal (non-trivial RMS / peak / span) and drifts
 * window-to-window -- exactly what ``pdm_mic_demo``'s plausibility check needs to
 * report ``active=Y`` on a real signal rather than on the sparse fallback's
 * degenerate 0/-1 toggle.
 *
 * The value path is synthetic (ra8_emulator has no analog microphone); this is a
 * run-headless enabler, not a claim about a physical mic. Self-registers its
 * descriptor with the board_periph core from a file-scope constructor -- see
 * board_periph_block.h.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief PDM-IF register-window geometry (ra8_pdm_regs.h). */
typedef enum : uint64_t {
  k_pdm_base       = 0x40256000UL,  /**< PDM-IF block base (HUM Ch 49).      */
  k_pdm_span       = 0x400UL,       /**< Common bank + 3 channel banks.      */
  k_pdm_words      = 0x400UL / 4UL, /**< Backing-store word count.           */
  k_pdm_ch_count   = 3UL,           /**< PDM channels (CH0..CH2).            */
  k_pdm_ch0_off    = 0x100UL,       /**< Offset of the first channel bank.   */
  k_pdm_ch_stride  = 0x100UL,       /**< Bytes between channel banks.        */
  k_pdm_off_cstrtr = 0x00UL,        /**< PDCSTRTR common start trigger.      */
  k_pdm_off_cstptr = 0x04UL,        /**< PDCSTPTR common stop trigger.       */
  k_pdm_off_csr    = 0x10UL,        /**< PDCSR common channel status.        */
  k_pdm_ch_pddrcr  = 0xE0UL,        /**< CHn PDDRCR data-read control.       */
  k_pdm_ch_pddrr   = 0xE8UL,        /**< CHn PDDRR 20-bit PCM data read.     */
  k_pdm_ch_pddsr   = 0xECUL,        /**< CHn PDDSR data status (fill count). */
} pdm_map_t;

/** @brief PDM-IF register field masks / bits (ra8_pdm_regs.h). */
typedef enum : uint32_t {
  k_pdm_pddrcr_datre = 0x1U,        /**< PDDRCR[0] data-read enable.           */
  k_pdm_pddsr_num    = 0x000000FFU, /**< PDDSR[7:0] FIFO fill count field.     */
  k_pdm_pddrr_mask20 = 0x000FFFFFU, /**< PDDRR[19:0] 20-bit two's-complement.  */
  k_pdm_fifo_depth   = 32U,         /**< Samples reported available when live. */
} pdm_field_t;

/** @brief Synthetic-tone shape constants (deterministic, integer-only). */
typedef enum : uint32_t {
  k_pdm_tone_period = 64U, /**< Samples per triangle cycle (power of two). */
  k_pdm_tone_qmask  = 63U, /**< period-1: phase wrap mask.                 */
  k_pdm_tone_half   = 32U, /**< period/2: rising-then-falling split point. */
} pdm_tone_t;

/** @brief Synthetic-tone amplitude / slope (20-bit signed, well below +-524288). */
typedef enum : int32_t {
  k_pdm_tone_amp  = 20000, /**< Triangle peak amplitude (+-).          */
  k_pdm_tone_step = 1250,  /**< Per-sample slope = 2*amp / (period/2). */
} pdm_amp_t;

/** @brief Per-channel pseudo-random dither (window-to-window variety). */
typedef enum : uint32_t {
  k_pdm_lcg_mul     = 1664525U,    /**< LCG multiplier (Numerical Recipes). */
  k_pdm_lcg_add     = 1013904223U, /**< LCG increment (Numerical Recipes).  */
  k_pdm_lcg_shift   = 16U,         /**< Take the high bits (better spread). */
  k_pdm_dither_mask = 0x3FFU,      /**< 10-bit dither window (0..1023).     */
  k_pdm_dither_half = 512U,        /**< Centre the dither at 0 (+-512).     */
} pdm_dither_t;

/** @brief Console anti-flood cadence for the PDM (I2S/audio) tab. */
typedef enum : uint32_t {
  k_pdm_console_line_cap = 48U,  /**< Max chars in a "PDM.. samples=.." line.  */
  k_pdm_console_every    = 512U, /**< Push one line per N samples per channel. */
} pdm_console_t;

/** @brief One modelled PDM channel: run/enable gates + synthetic-tone state. */
typedef struct {
  bool     running;     /**< A PDCSTRTR bit started this channel.           */
  bool     read_enable; /**< PDDRCR.DATRE armed the data-read path.         */
  uint32_t phase;       /**< Triangle-tone sample phase (advances on read). */
  uint32_t lcg;         /**< Per-channel dither LCG state.                  */
  uint32_t samples;     /**< Total PDDRR reads served (report + console).   */
} pdm_channel_t;

static pdm_channel_t s_pdm[k_pdm_ch_count];
static uint32_t      s_pdm_reg[k_pdm_words]; /**< Reflect-on-read shadow. */

/** @brief True when channel @p ch is both started and read-enabled. */
static bool pdm_live(uint32_t ch)
{
  if (ch >= (uint32_t)k_pdm_ch_count) {
    return false;
  }
  return s_pdm[ch].running && s_pdm[ch].read_enable;
}

/** @brief Next 20-bit two's-complement PCM sample for a live channel. */
static uint32_t pdm_next_sample(uint32_t ch)
{
  pdm_channel_t* c   = &s_pdm[ch];
  const uint32_t ph  = c->phase & (uint32_t)k_pdm_tone_qmask;
  int32_t        tri = 0;
  if (ph < (uint32_t)k_pdm_tone_half) {
    tri = -(int32_t)k_pdm_tone_amp + ((int32_t)k_pdm_tone_step * (int32_t)ph);
  } else {
    tri = (int32_t)k_pdm_tone_amp -
          ((int32_t)k_pdm_tone_step * (int32_t)(ph - (uint32_t)k_pdm_tone_half));
  }
  c->lcg = (c->lcg * (uint32_t)k_pdm_lcg_mul) + (uint32_t)k_pdm_lcg_add;
  const int32_t dither =
    (int32_t)((c->lcg >> (uint32_t)k_pdm_lcg_shift) & (uint32_t)k_pdm_dither_mask) -
    (int32_t)k_pdm_dither_half;
  c->phase++;
  c->samples++;
  const int32_t sample = tri + dither;
  return (uint32_t)sample & (uint32_t)k_pdm_pddrr_mask20;
}

/** @brief Push an anti-flood "PDMn samples=.." line to the I2S/audio tab. */
static void pdm_console_maybe(uint32_t ch)
{
  const uint32_t n = s_pdm[ch].samples;
  if ((n != 1U) && ((n % (uint32_t)k_pdm_console_every) != 0U)) {
    return;
  }
  char ln[k_pdm_console_line_cap];
  (void)snprintf(ln, sizeof(ln), "PDM%u samples=%u (synth tone)", (unsigned)ch, (unsigned)n);
  board_console_push(k_board_console_ch_i2s, ln);
}

/** @brief Compute the channel index + in-bank offset for an in-window address. */
static bool pdm_channel_at(uint64_t off, uint32_t* ch, uint64_t* ch_off)
{
  if (off < (uint64_t)k_pdm_ch0_off) {
    return false;
  }
  const uint64_t rel = off - (uint64_t)k_pdm_ch0_off;
  *ch                = (uint32_t)(rel / (uint64_t)k_pdm_ch_stride);
  *ch_off            = rel % (uint64_t)k_pdm_ch_stride;
  return (*ch < (uint32_t)k_pdm_ch_count);
}

/** @brief PDCSR: one run bit per currently-started channel. */
static uint32_t pdm_read_csr(void)
{
  uint32_t csr = 0U;
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_ch_count; i++) {
    if (s_pdm[i].running) {
      csr |= (1U << i);
    }
  }
  return csr;
}

/** @brief MMIO read inside the PDM window. */
static uint64_t pdm_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_pdm_base;
  if (off >= (uint64_t)k_pdm_span) {
    return 0U;
  }
  if (off == (uint64_t)k_pdm_off_csr) {
    return pdm_read_csr();
  }
  uint32_t ch     = 0U;
  uint64_t ch_off = 0U;
  if (pdm_channel_at(off, &ch, &ch_off)) {
    if (ch_off == (uint64_t)k_pdm_ch_pddsr) {
      return pdm_live(ch) ? (uint64_t)((uint32_t)k_pdm_fifo_depth & (uint32_t)k_pdm_pddsr_num) : 0U;
    }
    if (ch_off == (uint64_t)k_pdm_ch_pddrr) {
      if (!pdm_live(ch)) {
        return 0U;
      }
      const uint32_t sample = pdm_next_sample(ch);
      pdm_console_maybe(ch);
      return (uint64_t)sample;
    }
  }
  return (uint64_t)s_pdm_reg[off >> 2U];
}

/** @brief MMIO write inside the PDM window: latch, then update run/enable gates. */
static void pdm_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_pdm_base;
  if (off >= (uint64_t)k_pdm_span) {
    return;
  }
  s_pdm_reg[off >> 2U] = (uint32_t)value;

  if (off == (uint64_t)k_pdm_off_cstrtr) {
    for (uint32_t i = 0U; i < (uint32_t)k_pdm_ch_count; i++) {
      if (((uint32_t)value & (1U << i)) != 0U) {
        s_pdm[i].running = true;
      }
    }
    return;
  }
  if (off == (uint64_t)k_pdm_off_cstptr) {
    for (uint32_t i = 0U; i < (uint32_t)k_pdm_ch_count; i++) {
      if (((uint32_t)value & (1U << i)) != 0U) {
        s_pdm[i].running = false;
      }
    }
    return;
  }
  uint32_t ch     = 0U;
  uint64_t ch_off = 0U;
  if (pdm_channel_at(off, &ch, &ch_off) && (ch_off == (uint64_t)k_pdm_ch_pddrcr)) {
    s_pdm[ch].read_enable = (((uint32_t)value & (uint32_t)k_pdm_pddrcr_datre) != 0U);
  }
}

/** @brief Clear all PDM channel + shadow state to power-on. */
static void pdm_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_ch_count; i++) {
    s_pdm[i] = (pdm_channel_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_words; i++) {
    s_pdm_reg[i] = 0U;
  }
}

/** @brief Print one line per PDM channel the firmware captured from. */
static void pdm_report(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_pdm_ch_count; i++) {
    if (s_pdm[i].samples == 0U) {
      continue;
    }
    (void)fprintf(stderr,
                  "  PDM ch%u       : samples=%u run=%s read_en=%s (synth tone)\n",
                  i,
                  s_pdm[i].samples,
                  s_pdm[i].running ? "yes" : "no",
                  s_pdm[i].read_enable ? "yes" : "no");
  }
}

/** @brief This block's descriptor (static lifetime; the core keeps the pointer). */
static const board_periph_block_t k_pdm_block = {
  .base   = (uint64_t)k_pdm_base,
  .span   = (uint64_t)k_pdm_span,
  .order  = (uint32_t)k_block_order_i2c, /* Pull model (read-driven); no tick. */
  .read   = pdm_read,
  .write  = pdm_write,
  .tick   = nullptr,
  .reset  = pdm_reset,
  .report = pdm_report,
  .name   = "PDM-IF",
};

/** @brief Self-register the PDM-IF block before main runs (decentralized). */
[[gnu::constructor]] static void board_periph_pdm_register(void)
{
  board_periph_register_block(&k_pdm_block);
}
