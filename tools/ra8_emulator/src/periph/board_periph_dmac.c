/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_periph_dmac.c
 * @brief DMAC0 transfer peripheral-block model for the board emulator
 *
 * @details
 * Models the RA8D2 Direct Memory Access Controller (DMAC0) closely enough that
 * a software-triggered block copy actually MOVES bytes in emulated memory, so
 * an example such as @c dma_memcopy_demo -- which programmes a channel, fires a
 * software request, then verifies the destination buffer equals the source --
 * passes its @c dst==src check against real data rather than a faked handshake.
 *
 *  - **DMAC0** (ra8_dmac_regs.h, ra8_dmac.c): eight channels at
 *    @c 0x4000A000, @c 0x40 bytes apart. The driver writes DMSAR (source),
 *    DMDAR (destination), DMCRA (transfer count -- low half is the running
 *    count), DMTMD (SZ = transfer width), DMAMD (SM/DM = source/dest address
 *    update mode) and arms the channel with DMCNT.DTE. A write to DMREQ.SWREQ
 *    on an armed channel is the software trigger: the model reads the channel's
 *    programmed source/destination/width/increment out of its own register
 *    shadow and copies the configured number of units between the two emulated
 *    addresses with @c uc_mem_read / @c uc_mem_write, advancing each address by
 *    the unit size when its increment mode says so. It then drains DMCRA to
 *    zero, latches DMSTS.DTIF, clears DMSTS.ACT (the transfer is synchronous in
 *    the model, so the demo's "poll ACT until idle" gate falls straight
 *    through), auto-clears SWREQ, and -- if DMINT.DTIE is set -- raises the
 *    channel's transfer-end ELC event through the core's ICU -> NVIC path so an
 *    interrupt-driven transfer also works.
 *
 * The DTC (Data Transfer Controller, ra8_dtc_regs.h) shares MSTPA22 with
 * DMAC0 but is modelled in its own file (board_periph_dtc.c), which owns the DTC
 * control window (@c 0x4000AC00) and the ELC software-event activation path.
 *
 * The shared DMA module-control bank (DMAST activation gate, DMCTL, DELSR ELC
 * links) at @c 0x4000A800 is a second window, a transparent shadow: the driver
 * sets DMAST.DMST before the per-channel DTE write, and the model honours the
 * readback. The two DMAC windows register two block descriptors with the core;
 * the per-channel transfer state and the shared registers all reset together
 * from the channel descriptor's reset hook.
 *
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_console.h"
#include "board_periph_block.h"

/** @brief Console-tap line buffer capacity for a DMAC transfer summary. */
typedef enum : uint32_t {
  k_dmac_console_line_cap = 48U, /**< Max chars in a "DMAC.. .. units" line. */
} dmac_console_t;

/** @brief Field mask for the DMAC block-count register. */
typedef enum : uint32_t { k_u16_mask = 0xFFFFU /**< U16 mask. */ } dmac_lit_t;

/**
 * @brief Per-tick order slot for the DMAC / DTC windows.
 *
 * @details
 * These blocks have no @c tick, so their order only affects @c reset / @c report
 * sequencing. The value sits above the existing ::board_periph_block_order_t
 * slots (gpio 10, timer 20, sci 30, i2c 40) with spacing to spare, so the DMAC
 * summary prints after the existing peripheral sections without renumbering them
 * -- and without editing the shared header the conflict-free rule forbids
 * touching.
 */
typedef enum : uint32_t {
  k_dmac_block_order = 50U, /**< DMAC / DMA / DTC reset+report order slot. */
} dmac_order_t;

/** @brief DMAC0 per-channel window geometry (ra8_dmac_regs.h). */
typedef enum : uint64_t {
  k_dmac_base   = 0x4000A000UL, /**< DMAC0 channel 0 base.   */
  k_dmac_stride = 0x40UL,       /**< Bytes per DMAC channel. */
  k_dmac_count  = 8UL,          /**< DMAC0 channel 0..7.     */
  k_dmac_span   = 0x40UL * 8UL, /**< DMAC span.              */
} dmac_geom_t;

/** @brief Shared DMA module-control bank geometry (R_DMA at 0x4000A800). */
typedef enum : uint64_t {
  k_dma_shared_base = 0x4000A800UL, /**< DMAST / DMCTL / DMECHR / DELSR. */
  k_dma_shared_span = 0xA0UL,       /**< FSP R_DMA_Type size.            */
} dma_shared_geom_t;

/** @brief Per-channel register byte offsets (subset the driver touches). */
typedef enum : uint64_t {
  k_dmac_off_dmsar = 0x00UL, /**< Source address (32-bit).          */
  k_dmac_off_dmdar = 0x04UL, /**< Destination address (32-bit).     */
  k_dmac_off_dmcra = 0x08UL, /**< Transfer count (low/high halves). */
  k_dmac_off_dmcrb = 0x0CUL, /**< Block transfer count.             */
  k_dmac_off_dmtmd = 0x10UL, /**< Transfer mode (SZ / MD / DTS).    */
  k_dmac_off_dmint = 0x13UL, /**< Interrupt setting (DTIE et al.).  */
  k_dmac_off_dmamd = 0x14UL, /**< Address mode (SM / DM).           */
  k_dmac_off_dmofr = 0x18UL, /**< Offset register.                  */
  k_dmac_off_dmcnt = 0x1CUL, /**< Transfer enable (DTE bit0).       */
  k_dmac_off_dmreq = 0x1DUL, /**< Software start (SWREQ / CLRS).    */
  k_dmac_off_dmsts = 0x1EUL, /**< Status (ESIF / DTIF / ACT).       */
} dmac_off_t;

/** @brief DMCNT / DMREQ / DMSTS bit masks (ra8_dmac_regs.h). */
typedef enum : uint8_t {
  k_dmac_dte_mask   = 0x01U, /**< DMCNT.DTE channel-enable.     */
  k_dmac_swreq_mask = 0x01U, /**< DMREQ.SWREQ software request. */
  k_dmac_dtif_mask  = 0x10U, /**< DMSTS.DTIF transfer-end flag. */
  k_dmac_act_mask   = 0x80U, /**< DMSTS.ACT active flag.        */
} dmac_bit_t;

/** @brief DMTMD.SZ transfer-width field (bits 8..9) and unit sizes. */
typedef enum : uint32_t {
  k_dmac_sz_pos    = 8U,   /**< DMTMD.SZ field position.         */
  k_dmac_sz_mask   = 0x3U, /**< DMTMD.SZ field width (2 bits).   */
  k_dmac_sz_byte   = 0x0U, /**< 00b: 8-bit unit.                 */
  k_dmac_sz_half   = 0x1U, /**< 01b: 16-bit unit.                */
  k_dmac_sz_word   = 0x2U, /**< 10b: 32-bit unit.                */
  k_dmac_unit_byte = 1U,   /**< Bytes moved per byte-width unit. */
  k_dmac_unit_half = 2U,   /**< Bytes moved per half-width unit. */
  k_dmac_unit_word = 4U,   /**< Bytes moved per word-width unit. */
} dmac_sz_t;

/** @brief DMAMD address-update fields (ra8_dmac_regs.h). */
typedef enum : uint32_t {
  k_dmac_dm_pos  = 6U,   /**< DMAMD.DM destination-update position. */
  k_dmac_sm_pos  = 14U,  /**< DMAMD.SM source-update position.      */
  k_dmac_am_mask = 0x3U, /**< Address-update field width (2 bits).  */
  k_dmac_am_incr = 0x2U, /**< 10b: increment after each unit.       */
} dmac_am_t;

/** @brief DMCRA low half holds the running transfer count (bits 0..9). */
typedef enum : uint32_t {
  k_dmac_dmcra_low_mask = 0x3FFU, /**< DMCRA.DMCRAL count field. */
} dmac_dmcra_t;

/** @brief A bounded ceiling on units copied per trigger (safety net). */
typedef enum : uint32_t {
  k_dmac_max_units = 0x10000U, /**< Largest one-trigger unit count. */
} dmac_limit_t;

/**
 * @brief DMAC0 transfer-end ELC event the model emits (FSP ra8d2 bsp_elc).
 *
 * @details
 * The RA8D2 ELC signal table (HUM Ch 19) places the DMAC0 channel-0 transfer
 * interrupt (DMAC0_INT) at 0x0E0; FSP @c bsp_elc.h lists the eight DMAC channel
 * events contiguously from there. A firmware that routes the channel's
 * transfer-end interrupt through @c ra8_isr_register writes the matching number
 * into an IELSR slot, so the ICU model links the raised event to that slot.
 */
typedef enum : uint16_t {
  k_dmac_event_ch0 = 0x0E0U, /**< DMAC0 channel-0 transfer-end event. */
} dmac_event_t;

/** @brief One DMAC channel's modelled register shadow. */
typedef struct {
  uint32_t dmsar;  /**< DMSAR source address.         */
  uint32_t dmdar;  /**< DMDAR destination address.    */
  uint32_t dmcra;  /**< DMCRA transfer count.         */
  uint32_t dmcrb;  /**< DMCRB block transfer count.   */
  uint32_t dmofr;  /**< DMOFR offset register.        */
  uint16_t dmtmd;  /**< DMTMD transfer mode.          */
  uint16_t dmamd;  /**< DMAMD address mode.           */
  uint8_t  dmint;  /**< DMINT interrupt setting.      */
  uint8_t  dmcnt;  /**< DMCNT transfer enable.        */
  uint8_t  dmsts;  /**< DMSTS status flags.           */
  uint32_t copies; /**< Completed transfers (report). */
  uint32_t units;  /**< Units moved by the last copy. */
} dmac_chan_t;

static dmac_chan_t s_dmac[k_dmac_count];

/** @brief Shared DMA module-control bank shadow (DMAST / DMCTL / DELSR). */
static uint8_t s_dma_shared[k_dma_shared_span];

/* =============================================================================
 * DMAC channel transfer primitive (the part that actually moves memory).
 * =============================================================================
 */

/** @brief Bytes moved per unit for a DMTMD.SZ code (defaults to byte width). */
static uint32_t dmac_unit_bytes(uint32_t sz_code)
{
  if (sz_code == (uint32_t)k_dmac_sz_word) {
    return (uint32_t)k_dmac_unit_word;
  }
  if (sz_code == (uint32_t)k_dmac_sz_half) {
    return (uint32_t)k_dmac_unit_half;
  }
  return (uint32_t)k_dmac_unit_byte;
}

/** @brief Total units one software trigger transfers for channel @p c. */
static uint32_t dmac_total_units(const dmac_chan_t* c)
{
  uint32_t count = c->dmcra & (uint32_t)k_dmac_dmcra_low_mask;
  if (count == 0U) {
    count = (uint32_t)k_dmac_dmcra_low_mask + 1U; /* DMCRAL==0 means 1024 units */
  }
  uint32_t blocks = c->dmcrb & (uint32_t)k_u16_mask;
  if (blocks == 0U) {
    blocks = 1U; /* normal mode: a single block of `count` units */
  }
  uint64_t total = (uint64_t)count * (uint64_t)blocks;
  if (total > (uint64_t)k_dmac_max_units) {
    total = (uint64_t)k_dmac_max_units;
  }
  return (uint32_t)total;
}

/** @brief Copy @p units of @p unit bytes src->dst in emulated memory. */
static void dmac_copy_units(uc_engine* uc, dmac_chan_t* c, uint32_t units, uint32_t unit)
{
  uint64_t       src     = (uint64_t)c->dmsar;
  uint64_t       dst     = (uint64_t)c->dmdar;
  const uint32_t sm      = (c->dmamd >> (uint32_t)k_dmac_sm_pos) & (uint32_t)k_dmac_am_mask;
  const uint32_t dm      = (c->dmamd >> (uint32_t)k_dmac_dm_pos) & (uint32_t)k_dmac_am_mask;
  const bool     src_inc = (sm == (uint32_t)k_dmac_am_incr);
  const bool     dst_inc = (dm == (uint32_t)k_dmac_am_incr);
  uint8_t        word[k_dmac_unit_word] = {};
  for (uint32_t i = 0U; i < units; i++) {
    if (uc_mem_read(uc, src, word, unit) != UC_ERR_OK) {
      break;
    }
    if (uc_mem_write(uc, dst, word, unit) != UC_ERR_OK) {
      break;
    }
    if (src_inc) {
      src += unit;
    }
    if (dst_inc) {
      dst += unit;
    }
    c->units++;
  }
  c->dmsar = (uint32_t)src;
  c->dmdar = (uint32_t)dst;
}

/** @brief Run one software-triggered transfer on channel @p ch. */
static void dmac_run_transfer(uc_engine* uc, uint32_t ch)
{
  dmac_chan_t* c = &s_dmac[ch];
  if ((c->dmcnt & (uint8_t)k_dmac_dte_mask) == 0U) {
    return; /* channel disarmed: SWREQ does nothing */
  }
  const uint32_t sz    = (c->dmtmd >> (uint32_t)k_dmac_sz_pos) & (uint32_t)k_dmac_sz_mask;
  const uint32_t unit  = dmac_unit_bytes(sz);
  const uint32_t units = dmac_total_units(c);
  c->units             = 0U;
  dmac_copy_units(uc, c, units, unit);
  c->dmcra &= ~(uint32_t)k_dmac_dmcra_low_mask; /* count drained to zero        */
  c->dmsts &= (uint8_t)~k_dmac_act_mask;        /* synchronous: idle after copy */
  c->dmsts |= (uint8_t)k_dmac_dtif_mask;        /* transfer-end status latched  */
  c->copies++;
  /* Console DMA tab: one line per completed DMAC transfer (not per unit). */
  char ln[k_dmac_console_line_cap];
  (void)snprintf(ln,
                 sizeof(ln),
                 "DMAC%u %u units x %uB",
                 (unsigned)ch,
                 (unsigned)c->units,
                 (unsigned)unit);
  board_console_push(k_board_console_ch_dma, ln);
  if (board_periph_trace()) {
    (void)fprintf(stderr, "  DMAC%u         : copied %u units x %uB (SWREQ)\n", ch, c->units, unit);
  }
  if ((c->dmint & (uint8_t)k_dmac_dtif_mask) != 0U) {
    board_periph_icu_raise_event(uc, (uint16_t)((uint16_t)k_dmac_event_ch0 + (uint16_t)ch));
  }
}

/* =============================================================================
 * DMAC0 per-channel MMIO.
 * =============================================================================
 */

/** @brief Dispatch a DMAC channel register read for channel @p ch. */
static uint64_t dmac_reg_read(uint32_t ch, uint64_t off)
{
  const dmac_chan_t* c = &s_dmac[ch];
  switch (off) {
    case (uint64_t)k_dmac_off_dmsar:
      return c->dmsar;
    case (uint64_t)k_dmac_off_dmdar:
      return c->dmdar;
    case (uint64_t)k_dmac_off_dmcra:
      return c->dmcra;
    case (uint64_t)k_dmac_off_dmcrb:
      return c->dmcrb;
    case (uint64_t)k_dmac_off_dmtmd:
      return c->dmtmd;
    case (uint64_t)k_dmac_off_dmamd:
      return c->dmamd;
    case (uint64_t)k_dmac_off_dmofr:
      return c->dmofr;
    case (uint64_t)k_dmac_off_dmint:
      return c->dmint;
    case (uint64_t)k_dmac_off_dmcnt:
      return c->dmcnt;
    case (uint64_t)k_dmac_off_dmsts:
      return c->dmsts;
    default:
      return 0U;
  }
}

/** @brief Apply a DMAC channel register write (non-trigger registers). */
static void dmac_reg_store(dmac_chan_t* c, uint64_t off, uint32_t value)
{
  switch (off) {
    case (uint64_t)k_dmac_off_dmsar:
      c->dmsar = value;
      break;
    case (uint64_t)k_dmac_off_dmdar:
      c->dmdar = value;
      break;
    case (uint64_t)k_dmac_off_dmcra:
      c->dmcra = value;
      break;
    case (uint64_t)k_dmac_off_dmcrb:
      c->dmcrb = value;
      break;
    case (uint64_t)k_dmac_off_dmtmd:
      c->dmtmd = (uint16_t)value;
      break;
    case (uint64_t)k_dmac_off_dmamd:
      c->dmamd = (uint16_t)value;
      break;
    case (uint64_t)k_dmac_off_dmofr:
      c->dmofr = value;
      break;
    case (uint64_t)k_dmac_off_dmint:
      c->dmint = (uint8_t)value;
      break;
    case (uint64_t)k_dmac_off_dmcnt:
      c->dmcnt = (uint8_t)value;
      break;
    case (uint64_t)k_dmac_off_dmsts:
      /* ESIF/DTIF are write-0-to-clear; keep only bits still set. */
      c->dmsts &= (uint8_t)value;
      break;
    default:
      break;
  }
}

/** @brief Dispatch a DMAC channel register write; DMREQ.SWREQ triggers a copy. */
static void dmac_reg_write(uc_engine* uc, uint32_t ch, uint64_t off, uint32_t value)
{
  if (off == (uint64_t)k_dmac_off_dmreq) {
    if ((value & (uint32_t)k_dmac_swreq_mask) != 0U) {
      dmac_run_transfer(uc, ch); /* SWREQ auto-clears once accepted */
    }
    return;
  }
  dmac_reg_store(&s_dmac[ch], off, value);
}

/** @brief MMIO read inside the DMAC0 channel window. */
static uint64_t dmac_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_dmac_base) / (uint64_t)k_dmac_stride);
  return dmac_reg_read(ch, (addr - (uint64_t)k_dmac_base) % (uint64_t)k_dmac_stride);
}

/** @brief MMIO write inside the DMAC0 channel window. */
static void dmac_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_dmac_base) / (uint64_t)k_dmac_stride);
  dmac_reg_write(uc, ch, (addr - (uint64_t)k_dmac_base) % (uint64_t)k_dmac_stride, (uint32_t)value);
}

/* =============================================================================
 * Shared DMA module-control bank (transparent shadow).
 * =============================================================================
 */

/** @brief Read @p size bytes little-endian from a byte-array shadow. */
static uint64_t shadow_read(const uint8_t* shadow, uint64_t span, uint64_t off, unsigned size)
{
  uint64_t v = 0U;
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b < span) {
      v |= (uint64_t)shadow[b] << (8U * i);
    }
  }
  return v;
}

/** @brief Write @p size bytes little-endian into a byte-array shadow. */
static void
shadow_write(uint8_t* shadow, uint64_t span, uint64_t off, unsigned size, uint64_t value)
{
  for (unsigned i = 0U; i < size; i++) {
    const uint64_t b = off + (uint64_t)i;
    if (b < span) {
      shadow[b] = (uint8_t)(value >> (8U * i));
    }
  }
}

/** @brief MMIO read inside the shared DMA module-control bank. */
static uint64_t dma_shared_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  return shadow_read(s_dma_shared,
                     (uint64_t)k_dma_shared_span,
                     addr - (uint64_t)k_dma_shared_base,
                     size);
}

/** @brief MMIO write inside the shared DMA module-control bank. */
static void dma_shared_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  shadow_write(s_dma_shared,
               (uint64_t)k_dma_shared_span,
               addr - (uint64_t)k_dma_shared_base,
               size,
               value);
}

/* =============================================================================
 * Reset / report (carried by the DMAC channel descriptor).
 * =============================================================================
 */

/** @brief Clear all DMAC channel state plus the shared module-control shadow. */
static void dmac_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_dmac_count; i++) {
    s_dmac[i] = (dmac_chan_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_dma_shared_span; i++) {
    s_dma_shared[i] = 0U;
  }
}

/** @brief Print one line per DMAC channel that completed any transfer. */
static void dmac_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_dmac_count; ch++) {
    if (s_dmac[ch].copies > 0U) {
      (void)fprintf(stderr,
                    "  DMAC%u         : transfers=%u last_units=%u DMSTS=0x%02X\n",
                    ch,
                    s_dmac[ch].copies,
                    s_dmac[ch].units,
                    s_dmac[ch].dmsts);
    }
  }
}

/* The DMAC channel descriptor carries reset + report covering every DMAC
 * window; the shared-bank and DTC descriptors only own their register windows.
 * Registration order between them is irrelevant (dispatch is by disjoint
 * range). */

/** @brief DMAC0 channel window + the combined reset / report. */
static const board_periph_block_t k_dmac_block = {
  .base   = (uint64_t)k_dmac_base,
  .span   = (uint64_t)k_dmac_span,
  .order  = (uint32_t)k_dmac_block_order,
  .read   = dmac_read,
  .write  = dmac_write,
  .tick   = nullptr,
  .reset  = dmac_reset,
  .report = dmac_report,
  .name   = "DMAC",
};

/** @brief Shared DMA module-control bank window (transparent shadow). */
static const board_periph_block_t k_dma_shared_block = {
  .base   = (uint64_t)k_dma_shared_base,
  .span   = (uint64_t)k_dma_shared_span,
  .order  = (uint32_t)k_dmac_block_order,
  .read   = dma_shared_read,
  .write  = dma_shared_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "DMA",
};

/** @brief Self-register the DMAC0 / shared module-control windows before main. */
[[gnu::constructor]] static void board_periph_dmac_register(void)
{
  board_periph_register_block(&k_dmac_block);
  board_periph_register_block(&k_dma_shared_block);
}
