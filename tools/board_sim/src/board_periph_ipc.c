/**
 * @file board_periph_ipc.c
 * @brief Inter-Processor Communication (IPC) peripheral-block model for board_sim
 *
 * @details
 * Models the maskable-IRQ event path of the RA8D2 IPC unit (ra8d2_ipc_regs.h,
 * ra_ipc.c) so the dual-core @c compile_on_m33 example can replace its busy
 * done-flag poll with a real interrupt-driven wake: the secondary Cortex-M33,
 * having finished the EPUB->RABOOK1 compile, pokes @c IPC0ISET0 (HUM Ch 3.2.11
 * "IPC0ISET0" p 215) and the primary Cortex-M85 -- idling in WFI -- takes the
 * IPC0 receive interrupt and re-checks the mailbox.
 *
 * Only the channel IRQ-event surface is modelled; the FIFO data path (TXD / RXD
 * / RDY / FULL) and the semaphores / NMI windows are not exercised by the event
 * wake, so reads there return 0 and writes are benign no-ops. Per channel the
 * model keeps the @c STA pending-IRQ shadow (HUM Ch 3.2.10 "IPC0STA0" p 214):
 *
 *  - A write to @c ISET (set-IRQ, +0x04) latches the written IRQ-line bits into
 *    @c STA and raises the receiving core's ELC event through the core's
 *    ICU -> NVIC path, exactly the cross-core poke real silicon performs. IPC0
 *    channels (CPU1 -> CPU0) raise ELC_EVENT_IPC_IRQ0 (0x05B); IPC1 channels
 *    (CPU0 -> CPU1) raise ELC_EVENT_IPC_IRQ1 (0x05C).
 *  - A read of @c STA (+0x00) returns the latched pending bits the receiver's
 *    ra_ipc_dispatch decodes.
 *  - A write to @c CLR (W1C, +0x10) clears the acknowledged IRQ-line bits; the
 *    FIFO-reset / error-clear bits in the same register are ignored (no FIFO).
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

/** @brief IPC register window geometry (ra8d2_ipc_regs.h). */
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
  k_ipc_reg_clr   = 0x10UL, /**< CLR clear register (W1C).            */
} ipc_reg_off_t;

/** @brief Channel-count and unit-split constants. */
typedef enum : uint32_t {
  k_ipc_ch_count   = 4U, /**< IPC0_0, IPC0_1, IPC1_0, IPC1_1.           */
  k_ipc_unit_split = 2U, /**< Channels < split are IPC0, the rest IPC1. */
} ipc_chan_t;

/** @brief Status-register masks and the receiving-core ELC event ids. */
typedef enum : uint32_t {
  k_ipc_sta_irq_mask = 0x000000FFU, /**< Eight maskable IRQ-line bits.   */
  k_ipc0_irq_event   = 0x05BU,      /**< ELC_EVENT_IPC_IRQ0 -> CPU0/M85. */
  k_ipc1_irq_event   = 0x05CU,      /**< ELC_EVENT_IPC_IRQ1 -> CPU1/M33. */
} ipc_status_t;

/**
 * @struct ipc_state_t
 * @brief Modelled IPC state: per-channel pending IRQ bits plus run counters.
 * @details @c sta mirrors each channel's STA pending-IRQ field; @c sends and
 *          @c wakes are diagnostics for the end-of-run report.
 * @invariant Only @ref k_ipc_sta_irq_mask bits of each @c sta entry are ever set.
 */
typedef struct {
  uint32_t sta[k_ipc_ch_count]; /**< Per-channel STA pending-IRQ shadow. */
  uint32_t sends;               /**< ISET writes that latched IRQ bits.  */
  uint32_t wakes;               /**< Receiving-core ELC events raised.   */
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

/** @brief Read an IPC register: STA returns its pending bits, the rest read 0. */
static uint64_t ipc_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ipc_base;
  uint32_t       ch  = 0U;
  uint32_t       reg = 0U;
  if (ipc_decode(off, &ch, &reg) && (reg == (uint32_t)k_ipc_reg_sta)) {
    return (uint64_t)s_ipc.sta[ch];
  }
  /* FIFO RXD, semaphores and the NMI window are not modelled: read as 0. */
  return 0U;
}

/** @brief Write an IPC register: ISET latches + raises, CLR clears (W1C). */
static void ipc_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)size;
  const uint64_t off = addr - (uint64_t)k_ipc_base;
  uint32_t       ch  = 0U;
  uint32_t       reg = 0U;
  if (!ipc_decode(off, &ch, &reg)) {
    return; /* semaphore / NMI / FIFO-data write: benign no-op */
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
  if (reg == (uint32_t)k_ipc_reg_clr) {
    /* W1C the acknowledged IRQ lines; FIFO-reset / error-clear bits no-op. */
    s_ipc.sta[ch] &= ~((uint32_t)value & (uint32_t)k_ipc_sta_irq_mask);
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

/** @brief Print the IPC send / wake totals when the path was exercised. */
static void ipc_report(void)
{
  if ((s_ipc.sends == 0U) && (s_ipc.wakes == 0U)) {
    return;
  }
  (void)fprintf(stderr, "  IPC           : sends=%u wakes=%u\n", s_ipc.sends, s_ipc.wakes);
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
