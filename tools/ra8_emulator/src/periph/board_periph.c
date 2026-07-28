/**
 * @file board_periph.c
 * @brief Peripheral-block registry core + ICU/NVIC routing for ra8_emulator
 *
 * @details
 * The framework half of the peripheral model: a dynamic registry of peripheral
 * blocks (board_periph_block.h) that the MMIO callbacks dispatch into by address
 * range, plus the cross-block machinery the blocks share -- the ICU IELSR
 * event-link table, the pending-IRQ ring, and the NVIC set-enable shadow. The
 * block IMPLEMENTATIONS live in their own files (board_periph_gpio.c,
 * board_periph_timer.c, board_periph_sci.c, board_periph_i2c.c); each
 * self-registers its descriptor from a constructor, so this core keeps NO
 * hand-maintained block list and a new block is just a new file + a CMake line.
 *
 * Dispatch order: a registered block (by disjoint address range), then the
 * USBFS model (board_usb.c, forwarded), then the ICU IELSR window the core owns.
 * Per-chunk tick and the end-of-run report walk the registry in ascending
 * descriptor order so the historical cadence / section order is preserved
 * regardless of constructor registration order.
 *
 * Design: this module owns no Unicorn engine of its own and takes no AppKit
 * dependency. main.c passes the engine in where the model must read or write
 * emulated memory / pend an NVIC line, so board_periph stays plain C and the
 * exception delivery stays in the one place that already models it.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_periph.h"

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "board_periph_mstp_internal.h"
#include "board_usb.h"
#include "board_usb_host.h"

/* =============================================================================
 * ICU / NVIC register map (addresses verified against libs/ra8_hal regs).
 * =============================================================================
 */

/** @brief ICU block geometry (ra8_icu_regs.h). */
typedef enum : uint64_t {
  k_icu_base      = 0x40006000UL,            /**< R_ICU base.                */
  k_icu_off_ielsr = 0x6300UL,                /**< IELSR[0..95], 32-bit each. */
  k_icu_ielsr_cnt = 96UL,                    /**< IELSR slot count on RA8D2. */
  k_icu_span      = 0x6300UL + (96UL * 4UL), /**< Icu span.                  */
} icu_map_t;

/** @brief ICU IELSR layout -- ra8_ielsr_bit_t / ra8_ielsr_mask_t. */
typedef enum : uint32_t {
  k_ielsr_iels_mask = 0x000003FFU, /**< IELS event-select field [9:0].    */
  k_ielsr_ir_bit    = 16U,         /**< IR interrupt status flag (W0C).   */
  k_ielsr_ir_mask   = 0x00010000U, /**< IR bit mask.                      */
  k_ielsr_dtce_mask = 0x01000000U, /**< DTCE: DTC activation enable [24]. */
} ielsr_field_t;

/** @brief Cortex-M NVIC register bases (PPB, read straight from memory). */
typedef enum : uint64_t {
  k_nvic_iser_base = 0xE000E100UL, /**< Interrupt Set-Enable array.  */
  k_nvic_ispr_base = 0xE000E200UL, /**< Interrupt Set-Pending array. */
  k_nvic_word_bits = 32UL,         /**< Lines per ISER/ISPR word.    */
} nvic_addr_t;

/** @brief Pending-IRQ ring + per-IRQ tracking sizing. */
typedef enum : uint32_t {
  k_irq_queue_len = 32U, /**< Pending-IRQ ring capacity.    */
  k_irq_track_max = 64U, /**< Distinct IRQ numbers tracked. */
} irq_tune_t;

/** @brief Registry capacity + the core's own report slot ordering. */
typedef enum : uint32_t {
  /* Sized with headroom over the current block population (41 as of the RTT
   * drain model): overflowing this cap silently un-modelled whichever block
   * happened to register last (the constructor link order), which surfaced as
   * a baffling family of app regressions (the XSPI NOR flash apps) rather
   * than an error. board_periph_register_block now also reports the drop. */
  k_block_max = 56U, /**< Max registered peripheral blocks. */
  /* The core prints its NVIC-IRQ section after SCI (order 30) and before the
   * touch line (order 40), so it slots its report at this synthetic order. */
  k_core_irq_report_order = 35U, /**< Where the IRQ report sits among blocks. */
} core_tune_t;

static bool s_trace; /**< --trace: log transitions + IRQs as they happen. */

/* Active emulation device -- RA8D2 by default so a run with no --device flag is
 * byte-for-behaviour unchanged. Gates the RA8P1-only NPU block in block_for_addr;
 * set once by main.c before the run loop and persisted across warm reboots
 * (board_periph_init does not touch it -- the part is fixed). */
static board_device_t s_device = k_board_device_ra8d2;

/* --usbhs-loop: when set, loop-only blocks (the USBHS host controller model)
 * own their register windows; when clear (default) they are skipped and fall
 * through to the sparse fallback, so a run without the flag is unchanged. Set
 * once by main.c after argument parsing; board_periph_init does not touch it. */
static bool s_usbhs_loop;

/* =============================================================================
 * Block registry -- decentralized: each block self-registers its descriptor.
 * =============================================================================
 */

static const board_periph_block_t* s_blocks[k_block_max];      /**< Registered blocks.   */
static uint32_t                    s_block_count;              /**< Live entries.        */
static uint8_t                     s_block_order[k_block_max]; /**< Tick/report order.   */
static bool                        s_order_built;              /**< s_block_order valid. */

void board_periph_register_block(const board_periph_block_t* block)
{
  if (block == nullptr) {
    return;
  }
  if (s_block_count >= (uint32_t)k_block_max) {
    /* NEVER drop a block silently: an un-registered block reads as an
     * unmodelled peripheral and fails its apps with no hint of the cause. */
    (void)fprintf(stderr,
                  "board_periph: block registry FULL (k_block_max=%u) -- "
                  "DROPPING '%s'; raise k_block_max\n",
                  (unsigned)k_block_max,
                  (block->name != nullptr) ? block->name : "?");
    return;
  }
  s_blocks[s_block_count] = block;
  s_block_count++;
  s_order_built = false; /* a new block invalidates the cached tick/report order */
}

/**
 * @brief Build @c s_block_order: registry indices sorted by descriptor order.
 *
 * @details
 * A stable insertion sort by ::board_periph_block_t::order so the per-chunk tick
 * and the end-of-run report visit blocks in a deterministic cadence regardless
 * of the constructor registration order (ties keep registration order). MMIO
 * dispatch does not use this -- blocks own disjoint address ranges.
 *
 * @return Nothing.
 * @since 0.1.0
 */
static void board_periph_build_order(void)
{
  for (uint32_t i = 0U; i < s_block_count; i++) {
    uint32_t j = i;
    while ((j > 0U) && (s_blocks[s_block_order[j - 1U]]->order > s_blocks[i]->order)) {
      s_block_order[j] = s_block_order[j - 1U];
      j--;
    }
    s_block_order[j] = (uint8_t)i;
  }
  s_order_built = true;
}

/** @brief True iff @p addr is inside [@p base, @p base + @p span). */
static bool in_range(uint64_t addr, uint64_t base, uint64_t span)
{
  return (addr >= base) && (addr < (base + span));
}

/**
 * @brief Last block returned by ::block_for_addr (one-entry dispatch cache).
 *
 * @details
 * The firmware's polled SD-over-Simple-SPI byte protocol hammers a single
 * peripheral block (the SCI window) thousands of times in a row, and every MMIO
 * callback would otherwise re-scan all ~28 registered blocks. Caching the last
 * owner and testing it first collapses that scan to a single ::in_range compare
 * on the overwhelmingly common same-block run. It only changes HOW the owning
 * block is located, never WHICH block answers, so the byte/flag stream the
 * firmware observes is unchanged.
 *
 * @note Cleared in ::board_periph_init so a warm reboot cannot reuse a stale
 *       pointer (the descriptor list is static, but this keeps the invariant
 *       explicit).
 * @warning Not thread-safe; the run loop is single-threaded.
 * @since 0.1.0
 */
static const board_periph_block_t* s_last_block;

void board_periph_set_device(board_device_t device)
{
  s_device     = (device == k_board_device_ra8p1) ? k_board_device_ra8p1 : k_board_device_ra8d2;
  s_last_block = nullptr; /* a device change can alter which block owns an address */
}

board_device_t board_periph_device(void)
{
  return s_device;
}

void board_periph_set_usbhs_loop(bool on)
{
  s_usbhs_loop = on;
  s_last_block = nullptr; /* the gate change alters which block owns 0x40351000 */
}

bool board_periph_usbhs_loop(void)
{
  return s_usbhs_loop;
}

/** @brief Whether block @p b is exposed on the active emulation device + run. */
static bool block_on_active_device(const board_periph_block_t* b)
{
  if (b->loop_only && !s_usbhs_loop) {
    return false; /* loop-only block, --usbhs-loop off: fall through to sparse. */
  }
  if (b->device == k_board_block_dev_ra8p1) {
    return s_device == k_board_device_ra8p1;
  }
  return true; /* k_board_block_dev_any: present on every device. */
}

/** @brief Find the registered block owning @p addr on the active device, or NULL. */
static const board_periph_block_t* block_for_addr(uint64_t addr)
{
  if ((s_last_block != nullptr) && in_range(addr, s_last_block->base, s_last_block->span)) {
    return s_last_block;
  }
  for (uint32_t i = 0U; i < s_block_count; i++) {
    if (in_range(addr, s_blocks[i]->base, s_blocks[i]->span)) {
      if (!block_on_active_device(s_blocks[i])) {
        continue; /* RA8P1-only block on a non-RA8P1 run: fall through to sparse. */
      }
      s_last_block = s_blocks[i];
      return s_blocks[i];
    }
  }
  return nullptr;
}

/* =============================================================================
 * Core-owned ICU / NVIC state.
 * =============================================================================
 */

/* ICU IELSR event-link table. The ICU registers live inside the callback-MMIO
 * peripheral window (0x40006xxx), so the model owns this state itself rather
 * than reading it back through the MMIO callback. The firmware's IELSR writes
 * (ra8_isr_register / ra8_icu_route) land here; icu_raise_event scans it. */
static uint32_t s_ielsr[k_icu_ielsr_cnt];

/* Pending-IRQ ring: the ICU pushes a line here when a linked, NVIC-enabled
 * event fires; the run loop pops it and the engine vectors it in. */
static uint32_t s_irq_ring[k_irq_queue_len];
static uint32_t s_irq_head;
static uint32_t s_irq_tail;
static uint32_t s_irq_taken[k_irq_track_max]; /**< Times each IRQ was taken. */
static uint32_t s_irq_total;                  /**< Total IRQs delivered.     */

/* NVIC set-enable shadow. The Cortex-M NVIC ISER/ICER registers are
 * set-enable / clear-enable (a written 1 sets / clears that line, a written 0
 * is ignored), but ra8_emulator maps the PPB as plain RAM, so a raw store of
 * "1 << bit" to ISER would clobber every other already-enabled line. main.c
 * decodes ISER/ICER writes and routes the set/clear bits here, where this
 * accumulating mask is the authoritative enable state the ICU model checks --
 * so a firmware that enables several NVIC lines (e.g. SCI RXI + TXI + TEI, or
 * the USB controller lines later) has them all stay enabled. */
typedef enum : uint32_t {
  k_nvic_enable_words = 8U, /**< 256 NVIC lines tracked (8 * 32). */
} nvic_shadow_t;
static uint32_t s_nvic_iser_shadow[k_nvic_enable_words];

/* =============================================================================
 * Small memory helpers (the model reads PPB / NVIC straight from emulated RAM).
 * =============================================================================
 */

/** @brief Read a 32-bit little-endian word from emulated memory. */
static uint32_t periph_rd32(uc_engine* uc, uint64_t addr)
{
  uint32_t v = 0U;
  (void)uc_mem_read(uc, addr, &v, sizeof(v));
  return v;
}

/** @brief True iff NVIC line @p irq is enabled in the set-enable shadow. */
static bool nvic_enabled(uc_engine* uc, uint32_t irq)
{
  (void)uc; /* enable state is the accumulating shadow, not raw PPB RAM */
  const uint32_t word = irq / 32U;
  if (word >= (uint32_t)k_nvic_enable_words) {
    return false;
  }
  return ((s_nvic_iser_shadow[word] >> (irq % 32U)) & 1U) != 0U;
}

/** @brief Mirror the pended line into NVIC ISPR so firmware can observe it. */
static void nvic_set_pending(uc_engine* uc, uint32_t irq)
{
  const uint64_t word = (uint64_t)k_nvic_ispr_base + ((uint64_t)(irq / 32U) * 4U);
  uint32_t       ispr = periph_rd32(uc, word);
  ispr |= (1U << (irq % 32U));
  (void)uc_mem_write(uc, word, &ispr, sizeof(ispr));
}

void board_periph_nvic_set_enable(uint32_t irq, bool enable)
{
  const uint32_t word = irq / 32U;
  if (word >= (uint32_t)k_nvic_enable_words) {
    return;
  }
  const uint32_t bit = 1U << (irq % 32U);
  if (enable) {
    s_nvic_iser_shadow[word] |= bit;
  } else {
    s_nvic_iser_shadow[word] &= ~bit;
  }
}

/* =============================================================================
 * ICU event routing -- link a peripheral event to an NVIC line and pend it.
 * =============================================================================
 */

/** @brief Push an IRQ onto the pending ring (drop silently if full). */
static void irq_ring_push(uint32_t irq)
{
  const uint32_t next = (s_irq_tail + 1U) % (uint32_t)k_irq_queue_len;
  if (next == s_irq_head) {
    return; /* ring full: the run loop drains it every boundary, so rare */
  }
  s_irq_ring[s_irq_tail] = irq;
  s_irq_tail             = next;
}

void board_periph_icu_raise_event(uc_engine* uc, uint16_t event)
{
  for (uint32_t slot = 0U; slot < (uint32_t)k_icu_ielsr_cnt; slot++) {
    if ((s_ielsr[slot] & (uint32_t)k_ielsr_iels_mask) != (uint32_t)event) {
      continue;
    }
    s_ielsr[slot] |= (uint32_t)k_ielsr_ir_mask; /* latch IR (W0C by handler) */
    if (nvic_enabled(uc, slot)) {
      nvic_set_pending(uc, slot);
      irq_ring_push(slot);
      if (s_trace) {
        (void)fprintf(stderr, "  [trace] ICU event 0x%03X -> NVIC IRQ %u pended\n", event, slot);
      }
    }
    return; /* one IELSR slot owns a given event */
  }
}

uint32_t board_periph_icu_dtc_slot(uint16_t event)
{
  for (uint32_t slot = 0U; slot < (uint32_t)k_icu_ielsr_cnt; slot++) {
    const bool links = (s_ielsr[slot] & (uint32_t)k_ielsr_iels_mask) == (uint32_t)event;
    const bool dtce  = (s_ielsr[slot] & (uint32_t)k_ielsr_dtce_mask) != 0U;
    if (links && dtce) {
      return slot;
    }
  }
  return (uint32_t)k_icu_ielsr_cnt;
}

/** @brief Index of the IELSR slot owning @p addr, or k_icu_ielsr_cnt if none. */
static uint32_t icu_ielsr_slot(uint64_t addr)
{
  const uint64_t lo = (uint64_t)k_icu_base + (uint64_t)k_icu_off_ielsr;
  const uint64_t hi = lo + ((uint64_t)k_icu_ielsr_cnt * 4U);
  if ((addr < lo) || (addr >= hi)) {
    return (uint32_t)k_icu_ielsr_cnt;
  }
  return (uint32_t)((addr - lo) / 4U);
}

/** @brief Dispatch an ICU IELSR read from the model's own event-link state. */
static uint64_t icu_read(uint64_t addr)
{
  const uint32_t slot = icu_ielsr_slot(addr);
  return (slot < (uint32_t)k_icu_ielsr_cnt) ? s_ielsr[slot] : 0U;
}

/** @brief Dispatch an ICU IELSR write; IR is W0C, the IELS/DTCE bits are RW. */
static void icu_write(uint64_t addr, uint32_t value)
{
  const uint32_t slot = icu_ielsr_slot(addr);
  if (slot >= (uint32_t)k_icu_ielsr_cnt) {
    return;
  }
  /* IR (bit 16) is WRITE-ZERO-to-clear, not write-one-to-clear: HUM Ch 14.2.17
   * p 547, "IR flag (Interrupt Status Flag)" -> [Clearing condition] -- "the IR
   * flag is cleared to 0 by writing 0". A written 0 clears the latched flag; a
   * written 1 leaves it set. Every other bit takes the written value.
   *
   * This model had the polarity inverted (RW1C), which silently satisfied a
   * driver that ORed a 1 into IR to "clear" it -- a write real silicon ignores.
   * The emulator therefore ran every ISR-driven app cleanly while the bench
   * live-locked in an interrupt storm (#170: lpm_periodic_idle emitted no UART
   * at all, stuck in IRQ0_Handler with IELSR0 = 0x00010080). Modelling the real
   * polarity makes the emulator reproduce that storm, so the bug cannot hide here
   * again. */
  const uint32_t ir_now  = s_ielsr[slot] & (uint32_t)k_ielsr_ir_mask;
  const uint32_t ir_keep = ((value & (uint32_t)k_ielsr_ir_mask) != 0U) ? ir_now : 0U;
  s_ielsr[slot]          = (value & ~(uint32_t)k_ielsr_ir_mask) | ir_keep;
}

/* The USBFS model (board_usb.c) pends its controller interrupt by asserting the
 * USBFS_INT ELC event; it goes through the same IELSR -> NVIC path as every
 * other peripheral, so board_periph hands it the event-raise via a thin wrapper
 * (the engine is the only extra argument the event-raise needs). */

/** @brief ICU event-raise hook handed to the USB model (see board_usb.h). */
static void usb_irq_raiser(uc_engine* uc, uint16_t event)
{
  board_periph_icu_raise_event(uc, event);
}

/* =============================================================================
 * Shared framework services published to the block files.
 * =============================================================================
 */

bool board_periph_trace(void)
{
  return s_trace;
}

/* =============================================================================
 * Lifecycle.
 * =============================================================================
 */

void board_periph_init(bool trace)
{
  s_trace      = trace;
  s_last_block = nullptr; /* drop the dispatch cache across a warm reboot */
  if (!s_order_built) {
    board_periph_build_order();
  }
  /* Reset every registered block to its power-on state. */
  for (uint32_t i = 0U; i < s_block_count; i++) {
    const board_periph_block_t* b = s_blocks[s_block_order[i]];
    if (b->reset != nullptr) {
      b->reset();
    }
  }
  /* Core ICU / NVIC bookkeeping. */
  for (uint32_t i = 0U; i < (uint32_t)k_icu_ielsr_cnt; i++) {
    s_ielsr[i] = 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_irq_track_max; i++) {
    s_irq_taken[i] = 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_nvic_enable_words; i++) {
    s_nvic_iser_shadow[i] = 0U;
  }
  s_irq_head  = 0U;
  s_irq_tail  = 0U;
  s_irq_total = 0U;
  /* Bring the USBFS controller model + virtual host up and wire its interrupt
   * pend through this module's ICU/NVIC path (Phase 3, issue #67). */
  board_usb_init(trace);
  board_usb_set_irq_raiser(usb_irq_raiser);
  /* USBHS HOST-mode model (self-loop peer of the USBFS device above). Dormant
   * until main.c grants the loop AND the firmware selects host mode. */
  board_usb_host_init(trace);
}

/* =============================================================================
 * MMIO dispatch -- route an address to its block, else report "not handled".
 * =============================================================================
 */

uint64_t board_periph_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled)
{
  *handled                          = true;
  const board_periph_block_t* block = block_for_addr(addr);
  if ((block != nullptr) && !block->observe) {
    if (board_mstp_addr_stopped(addr)) {
      /* Module-stopped peripheral: unclocked on silicon, so it reads 0 and the
       * modelled block must NOT answer (#405). This is the same fidelity shape
       * as the #247 graphics power domain -- a block whose enable was never
       * cancelled does nothing on hardware, and now nothing in the emulator. */
      board_mstp_note_gated_access(addr, false);
      return 0U;
    }
    return block->read(uc, addr, size);
  }
  /* An observe-only block (e.g. GLCDC) does not own its window: fall through so
   * the caller's sparse model answers the read, exactly as if it were
   * unmodelled. */
  {
    bool           usb_hit = false;
    const uint64_t usb_val = board_usb_read(uc, addr, size, &usb_hit);
    if (usb_hit) {
      return usb_val;
    }
  }
  {
    bool           usbh_hit = false;
    const uint64_t usbh_val = board_usb_host_read(uc, addr, size, &usbh_hit);
    if (usbh_hit) {
      return usbh_val;
    }
  }
  if (icu_ielsr_slot(addr) < (uint32_t)k_icu_ielsr_cnt) {
    return icu_read(addr);
  }
  *handled = false;
  return 0U;
}

void board_periph_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled)
{
  *handled                          = true;
  const board_periph_block_t* block = block_for_addr(addr);
  if (block != nullptr) {
    if (!block->observe && board_mstp_addr_stopped(addr)) {
      /* Module-stopped peripheral: unclocked on silicon, so the write is
       * dropped (#405). Reported handled -- the access is consumed and
       * discarded exactly as the hardware discards it (no fault, no flag). */
      board_mstp_note_gated_access(addr, true);
      return;
    }
    block->write(uc, addr, size, value);
    /* An observe-only block snoops the write but does not consume it: report
     * NOT handled so the caller's sparse model also records it (the panel
     * compositor reads the GLCDC registers back from that shadow). */
    *handled = !block->observe;
    return;
  }
  {
    bool usb_hit = false;
    board_usb_write(uc, addr, size, value, &usb_hit);
    if (usb_hit) {
      return;
    }
  }
  {
    bool usbh_hit = false;
    board_usb_host_write(uc, addr, size, value, &usbh_hit);
    if (usbh_hit) {
      return;
    }
  }
  if (icu_ielsr_slot(addr) < (uint32_t)k_icu_ielsr_cnt) {
    icu_write(addr, (uint32_t)value);
    return;
  }
  *handled = false;
}

void board_periph_tick(uc_engine* uc)
{
  for (uint32_t i = 0U; i < s_block_count; i++) {
    const board_periph_block_t* b = s_blocks[s_block_order[i]];
    if (b->tick != nullptr) {
      b->tick(uc);
    }
  }
  /* Step the virtual USB host: it advances the chapter-9 enumeration and pends
   * the USBFS interrupt through ::usb_irq_raiser when it delivers a SETUP. */
  board_usb_tick(uc);
}

bool board_periph_next_irq(uint32_t* out_irq)
{
  if (s_irq_head == s_irq_tail) {
    return false; /* ring empty */
  }
  *out_irq   = s_irq_ring[s_irq_head];
  s_irq_head = (s_irq_head + 1U) % (uint32_t)k_irq_queue_len;
  return true;
}

void board_periph_note_irq_taken(uint32_t irq)
{
  s_irq_total++;
  if (irq < (uint32_t)k_irq_track_max) {
    s_irq_taken[irq]++;
  }
  if (s_trace) {
    (void)fprintf(stderr, "  [trace] NVIC IRQ %u taken (real exception via VTOR)\n", irq);
  }
}

uint32_t board_periph_irq_count(uint32_t irq)
{
  if (irq >= (uint32_t)k_irq_track_max) {
    return 0U;
  }
  return s_irq_taken[irq];
}

uint32_t board_periph_irq_total(void)
{
  return s_irq_total;
}

/* =============================================================================
 * End-of-run summary -- each block prints its section in ascending order, with
 * the core's NVIC-IRQ section slotted between SCI and the touch line, then USB.
 * =============================================================================
 */

/** @brief Print the per-IRQ taken totals (or a "none fired" note). */
static void report_irqs(void)
{
  if (s_irq_total == 0U) {
    (void)fprintf(stderr, "  NVIC IRQs     : 0 taken (no event-linked, enabled line fired)\n");
    return;
  }
  (void)fprintf(stderr, "  NVIC IRQs     : %u taken  [", s_irq_total);
  bool first = true;
  for (uint32_t i = 0U; i < (uint32_t)k_irq_track_max; i++) {
    if (s_irq_taken[i] > 0U) {
      (void)fprintf(stderr, "%sIRQ%u x%u", first ? "" : " ", i, s_irq_taken[i]);
      first = false;
    }
  }
  (void)fprintf(stderr, "]\n");
}

void board_periph_report(uc_engine* uc)
{
  (void)uc;
  bool irq_done = false;
  for (uint32_t i = 0U; i < s_block_count; i++) {
    const board_periph_block_t* b = s_blocks[s_block_order[i]];
    /* The core's NVIC-IRQ section sits between SCI and the touch line. */
    if (!irq_done && (b->order >= (uint32_t)k_core_irq_report_order)) {
      report_irqs();
      irq_done = true;
    }
    if (b->report != nullptr) {
      b->report();
    }
  }
  if (!irq_done) {
    report_irqs();
  }
  board_usb_report();
  board_usb_host_report();
}
