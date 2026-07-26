/**
 * @file board_periph_ipc.c
 * @brief Inter-Processor Communication (IPC) peripheral-block model for board_sim
 *
 * @details
 * Models the maskable-IRQ event path of the RA8D2 IPC unit (ra8_ipc_regs.h,
 * ra8_ipc.c) so the dual-core @c compile_on_m33 example can replace its busy
 * done-flag poll with a real interrupt-driven wake: the secondary Cortex-M33,
 * having finished the EPUB->RABOOK1 compile, pokes @c IPC0ISET0 (HUM Ch 3.2.11
 * "IPC0ISET0" p 215) and the primary Cortex-M85 -- idling in WFI -- takes the
 * IPC0 receive interrupt and re-checks the mailbox.
 *
 * Two surfaces are modelled per channel; the semaphores / NMI windows are not
 * exercised by any consumer, so reads there return 0 and writes are benign
 * no-ops. Per channel the model keeps the @c STA pending-IRQ shadow plus a
 * 4-stage message FIFO (HUM Ch 3.1 "Overview" p 204 fixes the depth):
 *
 *  - A write to @c ISET (set-IRQ, +0x04) latches the written IRQ-line bits into
 *    @c STA and raises the receiving core's ELC event through the core's
 *    ICU -> NVIC path, exactly the cross-core poke real silicon performs. IPC0
 *    channels (CPU1 -> CPU0) raise ELC_EVENT_IPC_IRQ0 (0x05B); IPC1 channels
 *    (CPU0 -> CPU1) raise ELC_EVENT_IPC_IRQ1 (0x05C).
 *  - A read of @c STA (+0x00) returns the latched pending bits the receiver's
 *    ra8_ipc_dispatch decodes, plus the live FIFO status: RDY (bit 16, FIFO
 *    non-empty), FULL (bit 17), and the RERR / FERR sticky error latches
 *    (bits 24 / 25). HUM Ch 3.2.10 "IPC0STA0" p 214.
 *  - A write to @c TXD (+0x08) pushes one 32-bit word onto the channel FIFO
 *    (FERR latches on a write-while-full); a read of @c RXD (+0x0C) pops one
 *    word (RERR latches on a read-while-empty). This is the data path the
 *    dual-core ping-pong (cpu1_pingpong_ipc) exchanges its magic words over:
 *    the sender pushes on one engine, the receiver polls STA.RDY and pops on
 *    the other, against ONE shared model instance. HUM Ch 3.2.12 "IPC0TXD0"
 *    p 215 / Ch 3.2.13 "IPC0RXD0" p 216.
 *  - A write to @c CLR (W1C, +0x10) clears the acknowledged IRQ-line bits;
 *    RST (bit 16) empties the FIFO (dropping RDY / FULL), and RCLR / FCLR
 *    (bits 24 / 25) drop the sticky error latches. HUM Ch 3.2.14 "IPC0CLR0"
 *    p 216-217.
 *
 * The M33 reaches this window through the bit[28] non-secure peripheral alias
 * (0x50020000); the cpu1 engine maps that alias to the same models via the same
 * MMIO hooks, so an alias access dispatches here identically to a Secure one.
 *
 * The IPC has no time-based behaviour, so the block carries no @c tick.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"

/**
 * @brief Per-tick order slot for the IPC block.
 *
 * @details
 * The IPC carries no @c tick, so its order is never consulted for advance; the
 * value only has to be unique among the registered blocks. Placed above the RTC
 * slot (60) following the same decentralised-ordering convention.
 */
typedef enum : uint32_t {
  k_ipc_block_order = 62U, /**< IPC reset / report order slot. */
} ipc_order_t;

/** @brief IPC register window geometry (ra8_ipc_regs.h). */
typedef enum : uint64_t {
  k_ipc_base = 0x40020000UL, /**< IPC unit base (Secure alias).            */
  k_ipc_span = 0x140UL,      /**< Semaphores + NMI + four channel windows. */
} ipc_geom_t;

/** @brief Channel-window layout within the IPC register block. */
typedef enum : uint64_t {
  k_ipc_ch0_off   = 0xC0UL, /**< IPC0 channel 0 (FIFO00) window base. */
  k_ipc_ch_stride = 0x20UL, /**< Stride between consecutive channels. */
  k_ipc_reg_sta   = 0x00UL, /**< STA pending-IRQ register (read).     */
  k_ipc_reg_iset  = 0x04UL, /**< ISET set-IRQ register (write).       */
  k_ipc_reg_txd   = 0x08UL, /**< TXD FIFO transmit register (write).  */
  k_ipc_reg_rxd   = 0x0CUL, /**< RXD FIFO receive register (read).    */
  k_ipc_reg_clr   = 0x10UL, /**< CLR clear register (W1C).            */
} ipc_reg_off_t;

/** @brief Channel-count, unit-split and FIFO-depth constants. */
typedef enum : uint32_t {
  k_ipc_ch_count   = 4U, /**< IPC0_0, IPC0_1, IPC1_0, IPC1_1.           */
  k_ipc_unit_split = 2U, /**< Channels < split are IPC0, the rest IPC1. */
  k_ipc_fifo_depth = 4U, /**< 4-stage message FIFO (HUM Ch 3.1 p 204).  */
} ipc_chan_t;

/** @brief Status-register masks and the receiving-core ELC event ids. */
typedef enum : uint32_t {
  k_ipc_sta_irq_mask = 0x000000FFU, /**< Eight maskable IRQ-line bits.      */
  k_ipc_sta_rdy      = 0x00010000U, /**< STA.RDY: receive FIFO non-empty.   */
  k_ipc_sta_full     = 0x00020000U, /**< STA.FULL: message FIFO full.       */
  k_ipc_sta_rerr     = 0x01000000U, /**< STA.RERR: read-while-empty sticky. */
  k_ipc_sta_ferr     = 0x02000000U, /**< STA.FERR: write-while-full sticky. */
  k_ipc_clr_rst      = 0x00010000U, /**< CLR.RST: reset the message FIFO.   */
  k_ipc_clr_rclr     = 0x01000000U, /**< CLR.RCLR: drop the RERR sticky.    */
  k_ipc_clr_fclr     = 0x02000000U, /**< CLR.FCLR: drop the FERR sticky.    */
  k_ipc0_irq_event   = 0x05BU,      /**< ELC_EVENT_IPC_IRQ0 -> CPU0/M85.    */
  k_ipc1_irq_event   = 0x05CU,      /**< ELC_EVENT_IPC_IRQ1 -> CPU1/M33.    */
} ipc_status_t;

/**
 * @struct ipc_fifo_t
 * @brief One channel's 4-stage message FIFO plus its sticky error latches.
 * @details @c word is a ring buffer: @c rd indexes the oldest entry and
 *          @c count the fill level, so push appends at (rd + count) % depth
 *          and pop consumes at @c rd. @c rerr / @c ferr mirror the STA
 *          sticky error bits until a CLR RCLR / FCLR write drops them.
 * @invariant @c count <= ::k_ipc_fifo_depth and @c rd < ::k_ipc_fifo_depth.
 */
typedef struct {
  uint32_t word[k_ipc_fifo_depth]; /**< Ring storage (oldest at @c rd).  */
  uint32_t rd;                     /**< Index of the oldest queued word. */
  uint32_t count;                  /**< Queued words (0..depth).         */
  bool     rerr;                   /**< Read-while-empty sticky latch.   */
  bool     ferr;                   /**< Write-while-full sticky latch.   */
} ipc_fifo_t;

/**
 * @struct ipc_state_t
 * @brief Modelled IPC state: per-channel IRQ bits + FIFOs plus run counters.
 * @details @c sta mirrors each channel's STA pending-IRQ field and @c fifo its
 *          4-stage message FIFO; @c sends, @c wakes, @c pushes and @c pops are
 *          diagnostics for the end-of-run report.
 * @invariant Only @ref k_ipc_sta_irq_mask bits of each @c sta entry are ever set.
 */
typedef struct {
  uint32_t   sta[k_ipc_ch_count];  /**< Per-channel STA pending-IRQ shadow. */
  ipc_fifo_t fifo[k_ipc_ch_count]; /**< Per-channel 4-stage message FIFO.   */
  uint32_t   sends;                /**< ISET writes that latched IRQ bits.  */
  uint32_t   wakes;                /**< Receiving-core ELC events raised.   */
  uint32_t   pushes;               /**< TXD words accepted into a FIFO.     */
  uint32_t   pops;                 /**< RXD words drained from a FIFO.      */
} ipc_state_t;

/**
 * @var s_ipc
 * @brief Single shared IPC model instance (both engines dispatch into it).
 * @warning Mutated only from the board_sim MMIO hooks; not for direct use.
 */
static ipc_state_t s_ipc;

/* =============================================================================
 * Channel / register decode.
 * =============================================================================
 */

/**
 * @brief Decode a window offset to its channel index and register offset.
 *
 * @param[in]  off Byte offset from @ref k_ipc_base.
 * @param[out] ch  Receives the channel index 0..3 on success.
 * @param[out] reg Receives the in-channel register offset on success.
 *
 * @return Whether @p off lands inside a channel window.
 * @retval true  @p off is in [0xC0, 0x140); @p ch / @p reg are set.
 * @retval false @p off is in the semaphore / NMI region (no channel).
 *
 * @pre @p ch and @p reg are non-NULL.
 * @pre @p off is within the block span.
 * @post On true, @p ch < ::k_ipc_ch_count.
 * @post On false, @p ch and @p reg are untouched.
 *
 * @note Pure decode; touches no model state.
 * @since 0.1.0
 */
static bool ipc_decode(uint64_t off, uint32_t* ch, uint32_t* reg)
{
  if (off < (uint64_t)k_ipc_ch0_off) {
    return false;
  }
  const uint64_t rel     = off - (uint64_t)k_ipc_ch0_off;
  const uint32_t channel = (uint32_t)(rel / (uint64_t)k_ipc_ch_stride);
  if (channel >= (uint32_t)k_ipc_ch_count) {
    return false;
  }
  *ch  = channel;
  *reg = (uint32_t)(rel % (uint64_t)k_ipc_ch_stride);
  return true;
}

/* =============================================================================
 * MMIO read / write.
 * =============================================================================
 */

/**
 * @brief Compose one channel's live STA value from its shadows.
 *
 * @param[in] ch Channel index (0..::k_ipc_ch_count - 1).
 *
 * @return The STA read value: pending-IRQ bits, RDY / FULL FIFO status, and
 *         the RERR / FERR sticky error latches.
 * @retval 0 Channel idle: no pending IRQ, empty FIFO, no latched error.
 *
 * @pre @p ch is a decoded channel index (< ::k_ipc_ch_count).
 * @pre The model state ::s_ipc is initialised (constructor ran).
 * @post No model state is modified (pure compose).
 * @post RDY reflects @c count > 0 and FULL reflects @c count == depth.
 *
 * @note HUM Ch 3.2.10 "IPC0STA0" p 214 -- field layout.
 * @since 0.1.0
 */
static uint32_t ipc_sta_value(uint32_t ch)
{
  const ipc_fifo_t* f   = &s_ipc.fifo[ch];
  uint32_t          sta = s_ipc.sta[ch];
  if (f->count > 0U) {
    sta |= (uint32_t)k_ipc_sta_rdy;
  }
  if (f->count >= (uint32_t)k_ipc_fifo_depth) {
    sta |= (uint32_t)k_ipc_sta_full;
  }
  if (f->rerr) {
    sta |= (uint32_t)k_ipc_sta_rerr;
  }
  if (f->ferr) {
    sta |= (uint32_t)k_ipc_sta_ferr;
  }
  return sta;
}

/**
 * @brief Pop the oldest word off a channel's message FIFO (RXD read).
 *
 * @param[in] ch Channel index (0..::k_ipc_ch_count - 1).
 *
 * @return The dequeued word, or 0 with RERR latched on an empty FIFO.
 * @retval 0 FIFO was empty (RERR sticky latched) or the queued word was 0.
 *
 * @pre @p ch is a decoded channel index (< ::k_ipc_ch_count).
 * @pre The model state ::s_ipc is initialised.
 * @post On a non-empty FIFO one word is consumed and @c pops advances.
 * @post On an empty FIFO the RERR sticky latch is set.
 *
 * @note HUM Ch 3.2.13 "IPC0RXD0" p 216 -- a read pops one FIFO stage.
 * @since 0.1.0
 */
static uint32_t ipc_fifo_pop(uint32_t ch)
{
  ipc_fifo_t* f = &s_ipc.fifo[ch];
  if (f->count == 0U) {
    f->rerr = true; /* read-while-empty: latch RERR, data undefined (0). */
    return 0U;
  }
  const uint32_t w = f->word[f->rd];
  f->rd            = (f->rd + 1U) % (uint32_t)k_ipc_fifo_depth;
  f->count--;
  s_ipc.pops++;
  return w;
}

/** @brief Read an IPC register: STA composes live status, RXD pops the FIFO. */
static uint64_t ipc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ipc_base;
  uint32_t       ch  = 0U;
  uint32_t       reg = 0U;
  if (ipc_decode(off, &ch, &reg)) {
    if (reg == (uint32_t)k_ipc_reg_sta) {
      return (uint64_t)ipc_sta_value(ch);
    }
    if (reg == (uint32_t)k_ipc_reg_rxd) {
      return (uint64_t)ipc_fifo_pop(ch);
    }
  }
  /* Semaphores and the NMI window are not modelled: read as 0. */
  return 0U;
}

/**
 * @brief Push one word onto a channel's message FIFO (TXD write).
 *
 * @param[in] ch    Channel index (0..::k_ipc_ch_count - 1).
 * @param[in] value The 32-bit message word to queue.
 *
 * @return Nothing.
 *
 * @pre @p ch is a decoded channel index (< ::k_ipc_ch_count).
 * @pre The model state ::s_ipc is initialised.
 * @post On a non-full FIFO the word is appended and @c pushes advances;
 *       the receiver's next STA read shows RDY.
 * @post On a full FIFO the word is dropped and the FERR sticky latch is set.
 *
 * @note HUM Ch 3.2.12 "IPC0TXD0" p 215-216 -- a write pushes one FIFO stage.
 * @since 0.1.0
 */
static void ipc_fifo_push(uint32_t ch, uint32_t value)
{
  ipc_fifo_t* f = &s_ipc.fifo[ch];
  if (f->count >= (uint32_t)k_ipc_fifo_depth) {
    f->ferr = true; /* write-while-full: latch FERR, drop the word. */
    return;
  }
  f->word[(f->rd + f->count) % (uint32_t)k_ipc_fifo_depth] = value;
  f->count++;
  s_ipc.pushes++;
}

/**
 * @brief Apply a CLR write: W1C IRQ lines, FIFO reset, error-latch clears.
 *
 * @param[in] ch    Channel index (0..::k_ipc_ch_count - 1).
 * @param[in] value The written CLR word.
 *
 * @return Nothing.
 *
 * @pre @p ch is a decoded channel index (< ::k_ipc_ch_count).
 * @pre The model state ::s_ipc is initialised.
 * @post IRQ-line bits written as 1 are cleared from the STA shadow.
 * @post RST empties the FIFO; RCLR / FCLR drop the sticky error latches.
 *
 * @note HUM Ch 3.2.14 "IPC0CLR0" p 216-217 -- field layout.
 * @since 0.1.0
 */
static void ipc_clr_write(uint32_t ch, uint32_t value)
{
  s_ipc.sta[ch] &= ~(value & (uint32_t)k_ipc_sta_irq_mask);
  ipc_fifo_t* f = &s_ipc.fifo[ch];
  if ((value & (uint32_t)k_ipc_clr_rst) != 0U) {
    f->rd    = 0U;
    f->count = 0U; /* FIFO reset also drops RDY + FULL (they are computed). */
  }
  if ((value & (uint32_t)k_ipc_clr_rclr) != 0U) {
    f->rerr = false;
  }
  if ((value & (uint32_t)k_ipc_clr_fclr) != 0U) {
    f->ferr = false;
  }
}

/** @brief Write an IPC register: ISET latches + raises, TXD pushes, CLR clears. */
static void ipc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ipc_base;
  uint32_t       ch  = 0U;
  uint32_t       reg = 0U;
  if (!ipc_decode(off, &ch, &reg)) {
    return; /* semaphore / NMI window write: benign no-op */
  }
  if (reg == (uint32_t)k_ipc_reg_iset) {
    const uint32_t bits = (uint32_t)value & (uint32_t)k_ipc_sta_irq_mask;
    if (bits == 0U) {
      return;
    }
    s_ipc.sta[ch] |= bits;
    s_ipc.sends++;
    /* Raise the receiving core's ELC event: the core's ICU walks its IELSR
     * shadow, pends the mapped NVIC line, and the M85 takes it on its next
     * instruction boundary (cpu1 never drains, so an IPC1 raise is inert). */
    const uint16_t event =
      (ch < (uint32_t)k_ipc_unit_split) ? (uint16_t)k_ipc0_irq_event : (uint16_t)k_ipc1_irq_event;
    board_periph_icu_raise_event(uc, event);
    s_ipc.wakes++;
    if (board_periph_trace()) {
      (void)fprintf(stderr,
                    "  IPC           : ch%u ISET 0x%02X -> raise 0x%03X\n",
                    ch,
                    bits,
                    (unsigned)event);
    }
    return;
  }
  if (reg == (uint32_t)k_ipc_reg_txd) {
    ipc_fifo_push(ch, (uint32_t)value);
    return;
  }
  if (reg == (uint32_t)k_ipc_reg_clr) {
    ipc_clr_write(ch, (uint32_t)value);
  }
}

/* =============================================================================
 * Reset / report.
 * =============================================================================
 */

/** @brief Clear the IPC channel shadows and the run counters. */
static void ipc_reset(void)
{
  s_ipc = (ipc_state_t){};
}

/** @brief Print the IPC send / wake / FIFO totals when the path was exercised. */
static void ipc_report(void)
{
  if ((s_ipc.sends == 0U) && (s_ipc.wakes == 0U) && (s_ipc.pushes == 0U) && (s_ipc.pops == 0U)) {
    return;
  }
  (void)fprintf(stderr,
                "  IPC           : sends=%u wakes=%u fifo pushes=%u pops=%u\n",
                s_ipc.sends,
                s_ipc.wakes,
                s_ipc.pushes,
                s_ipc.pops);
}

/** @brief IPC register window + reset / report (no tick: event-driven only). */
static const board_periph_block_t k_ipc_block = {
  .base   = (uint64_t)k_ipc_base,
  .span   = (uint64_t)k_ipc_span,
  .order  = (uint32_t)k_ipc_block_order,
  .read   = ipc_read,
  .write  = ipc_write,
  .tick   = nullptr,
  .reset  = ipc_reset,
  .report = ipc_report,
  .name   = "IPC",
};

/** @brief Self-register the IPC window before main runs (decentralised). */
[[gnu::constructor]] static void board_periph_ipc_register(void)
{
  board_periph_register_block(&k_ipc_block);
}
