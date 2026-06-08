/**
 * @file board_periph.c
 * @brief Register-accurate peripheral models + ICU/NVIC routing for board_sim
 *
 * @details
 * Implements the framework declared in board_periph.h: a small table of
 * peripheral blocks (base / size / read / write / state) that the MMIO
 * callbacks dispatch into, superseding the sparse fallback for the modelled
 * registers. The blocks modelled here are GPIO/PORT, the AGT and GPT timers,
 * and the ICU (event-link -> NVIC pend). Each block keeps real state and the
 * timers advance a real counter per emulation chunk, so a non-display example
 * sees genuine peripheral behaviour and real interrupts -- not faked ready
 * bits.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include "board_periph.h"

#include <stdint.h>
#include <stdio.h>

/* =============================================================================
 * RA8D2 peripheral register map (addresses verified against libs/ra_hal regs).
 * =============================================================================
 */

/** @brief GPIO/PORT block geometry (ra8d2_port_regs.h). */
typedef enum : uint64_t {
  k_port_base   = 0x40400000UL,  /**< PORT0 base.                          */
  k_port_stride = 0x20UL,        /**< Bytes between adjacent ports.        */
  k_port_count  = 15UL,          /**< PORT0..PORT14.                       */
  k_port_span   = 0x20UL * 15UL, /**< Full PORT address window.            */
  k_port_pcntr1 = 0x00UL,        /**< {PODR[31:16], PDR[15:0]} RW.         */
  k_port_pcntr2 = 0x04UL,        /**< {EIDR[31:16], PIDR[15:0]} R.         */
  k_port_pcntr3 = 0x08UL,        /**< {PORR[31:16], POSR[15:0]} W.         */
  k_port_pcntr4 = 0x0CUL,        /**< {EORR[31:16], EOSR[15:0]} RW.        */
} port_map_t;

/** @brief AGT block geometry (ra8d2_agt_regs.h, 16-bit view). */
typedef enum : uint64_t {
  k_agt_base    = 0x40221000UL, /**< AGT0 base.                           */
  k_agt_stride  = 0x100UL,      /**< Bytes per AGT channel.               */
  k_agt_count   = 10UL,         /**< AGT0..AGT9.                          */
  k_agt_span    = 0x100UL * 10UL,
  k_agt_off_agt = 0x00UL, /**< AGT counter (16-bit down).           */
  k_agt_off_cma = 0x02UL, /**< AGTCMA compare-match A.              */
  k_agt_off_cmb = 0x04UL, /**< AGTCMB compare-match B.              */
  k_agt_off_cr  = 0x08UL, /**< AGTCR control/status (8-bit).        */
  k_agt_off_mr1 = 0x09UL, /**< AGTMR1 mode 1 (8-bit).               */
} agt_map_t;

/** @brief GPT block geometry (ra8d2_gpt_regs.h, 32-bit channels). */
typedef enum : uint64_t {
  k_gpt_base      = 0x40322000UL, /**< GPT0 base.                           */
  k_gpt_stride    = 0x100UL,      /**< Bytes per GPT channel.               */
  k_gpt_count     = 14UL,         /**< GPT0..GPT13.                         */
  k_gpt_span      = 0x100UL * 14UL,
  k_gpt_off_gtstr = 0x04UL, /**< GTSTR software start.                */
  k_gpt_off_gtstp = 0x08UL, /**< GTSTP software stop.                 */
  k_gpt_off_gtclr = 0x0CUL, /**< GTCLR software clear.                */
  k_gpt_off_gtcr  = 0x2CUL, /**< GTCR control (CST bit0).             */
  k_gpt_off_gtst  = 0x3CUL, /**< GTST status.                         */
  k_gpt_off_gtcnt = 0x48UL, /**< GTCNT counter (32-bit up).           */
  k_gpt_off_gtpr  = 0x64UL, /**< GTPR period.                         */
} gpt_map_t;

/** @brief ICU block geometry (ra8d2_icu_regs.h). */
typedef enum : uint64_t {
  k_icu_base      = 0x40006000UL, /**< R_ICU base.                         */
  k_icu_off_ielsr = 0x6300UL,     /**< IELSR[0..95], 32-bit each.          */
  k_icu_ielsr_cnt = 96UL,         /**< IELSR slot count on RA8D2.          */
  k_icu_span      = 0x6300UL + (96UL * 4UL),
} icu_map_t;

/* =============================================================================
 * Register bit fields and model constants.
 * =============================================================================
 */

/** @brief Generic field shifts / masks shared by the PORT halves. */
typedef enum : uint32_t {
  k_half_shift = 16U,     /**< High-half (PODR/PORR/EIDR) shift.     */
  k_half_mask  = 0xFFFFU, /**< 16-bit half mask.                     */
  k_u16_max    = 0xFFFFU, /**< 16-bit counter wrap value.            */
} field_t;

/** @brief AGTCR (control/status) bits -- ra_agt_agtcr_bits_t. */
typedef enum : uint32_t {
  k_agtcr_tstart = 0x01U, /**< TSTART start request.                 */
  k_agtcr_tcstf  = 0x02U, /**< TCSTF count-status flag (RO).         */
  k_agtcr_tundf  = 0x20U, /**< TUNDF underflow flag (RW1C).          */
  k_agtcr_tcmaf  = 0x40U, /**< TCMAF compare-match A flag (RW1C).    */
  k_agtcr_tcmbf  = 0x80U, /**< TCMBF compare-match B flag (RW1C).    */
} agtcr_bit_t;

/** @brief GPT GTCR / GTST bits -- ra_gpt register notes. */
typedef enum : uint32_t {
  k_gtcr_cst   = 0x00000001U, /**< GTCR.CST count-start.                 */
  k_gtst_tcfa  = 0x00000001U, /**< GTST.TCFA compare-match A.            */
  k_gtst_tcfpo = 0x00000040U, /**< GTST.TCFPO overflow.                  */
  k_gtst_tcfpu = 0x00000080U, /**< GTST.TCFPU underflow.                 */
} gpt_bit_t;

/** @brief ICU IELSR layout -- ra_ielsr_bit_t / ra_ielsr_mask_t. */
typedef enum : uint32_t {
  k_ielsr_iels_mask = 0x000003FFU, /**< IELS event-select field [9:0].    */
  k_ielsr_ir_bit    = 16U,         /**< IR interrupt status flag (RW1C).  */
  k_ielsr_ir_mask   = 0x00010000U, /**< IR bit mask.                      */
} ielsr_field_t;

/** @brief Cortex-M NVIC register bases (PPB, read straight from memory). */
typedef enum : uint64_t {
  k_nvic_iser_base = 0xE000E100UL, /**< Interrupt Set-Enable array.       */
  k_nvic_ispr_base = 0xE000E200UL, /**< Interrupt Set-Pending array.      */
  k_nvic_word_bits = 32UL,         /**< Lines per ISER/ISPR word.         */
} nvic_addr_t;

/**
 * @brief Canonical ELC event numbers the timer models emit (FSP ra8d2 bsp_elc).
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19): GPT0 overflow is 0x0C1 and AGT0
 * combined interrupt (underflow / compare-match) is 0x0DF. A firmware that
 * routes one of these through ra_isr_register writes the same number into an
 * IELSR slot, so the ICU model can match the raised event to that slot.
 */
typedef enum : uint16_t {
  k_event_gpt0_ovf = 0x0C1U, /**< GPT0 GTCIV counter-overflow event.      */
  k_event_agt0_int = 0x0DFU, /**< AGT0 AGTI combined interrupt event.     */
} elc_event_t;

/** @brief Per-chunk advance for the modelled counters (one chunk == 1 tick). */
typedef enum : uint32_t {
  k_agt_step_per_chunk = 0x0800U,     /**< AGT down-count per chunk.       */
  k_gpt_step_per_chunk = 0x00004000U, /**< GPT up-count per chunk.         */
  k_irq_queue_len      = 32U,         /**< Pending-IRQ ring capacity.      */
  k_irq_track_max      = 64U,         /**< Distinct IRQ numbers tracked.   */
} model_tune_t;

/* =============================================================================
 * Block state.
 * =============================================================================
 */

/** @brief One PORT instance: direction + output latch (16 bits each). */
typedef struct {
  uint16_t pdr;  /**< Direction: 1 = output, 0 = input. */
  uint16_t podr; /**< Output-data latch.                */
} port_state_t;

/** @brief One AGT channel: a 16-bit reloading down-counter + status. */
typedef struct {
  uint16_t counter;    /**< Live AGT count.            */
  uint16_t reload;     /**< Value last written to AGT. */
  uint16_t cmpa;       /**< AGTCMA compare-match A.    */
  uint16_t cmpb;       /**< AGTCMB compare-match B.    */
  uint8_t  cr;         /**< AGTCR control/status.      */
  uint8_t  mr1;        /**< AGTMR1 mode 1.             */
  uint32_t underflows; /**< Underflow event count.  */
} agt_state_t;

/** @brief One GPT channel: a 32-bit saw up-counter + status. */
typedef struct {
  uint32_t cnt;       /**< GTCNT live count.         */
  uint32_t period;    /**< GTPR period.              */
  uint32_t cr;        /**< GTCR (CST in bit0).       */
  uint32_t st;        /**< GTST status flags.        */
  uint32_t overflows; /**< Overflow event count.     */
} gpt_state_t;

static bool s_trace; /**< --trace: log transitions + IRQs as they happen. */

static port_state_t s_port[k_port_count];
static agt_state_t  s_agt[k_agt_count];
static gpt_state_t  s_gpt[k_gpt_count];

/** @brief Board LED -> (port index, pin index), mirrored from the BSP. */
typedef struct {
  uint8_t     port;
  uint8_t     pin;
  const char* name;
} led_map_t;

static const led_map_t k_led_map[k_board_led_count] = {
  {6U, 0U, "LED1 BLUE  P600"},
  {3U, 3U, "LED2 GREEN P303"},
  {10U, 7U, "LED3 RED   PA07"},
};

static uint32_t s_led_level[k_board_led_count];       /**< Last driven level. */
static uint32_t s_led_transitions[k_board_led_count]; /**< 0->1 / 1->0 count. */

/* ICU IELSR event-link table. The ICU registers live inside the callback-MMIO
 * peripheral window (0x40006xxx), so the model owns this state itself rather
 * than reading it back through the MMIO callback. The firmware's IELSR writes
 * (ra_isr_register / ra_icu_route) land here; icu_raise_event scans it. */
static uint32_t s_ielsr[k_icu_ielsr_cnt];

/* Pending-IRQ ring: the ICU pushes a line here when a linked, NVIC-enabled
 * event fires; the run loop pops it and the engine vectors it in. */
static uint32_t s_irq_ring[k_irq_queue_len];
static uint32_t s_irq_head;
static uint32_t s_irq_tail;
static uint32_t s_irq_taken[k_irq_track_max]; /**< Times each IRQ was taken. */
static uint32_t s_irq_total;                  /**< Total IRQs delivered.     */

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

/** @brief True iff NVIC line @p irq is enabled (ISER bit set) in PPB RAM. */
static bool nvic_enabled(uc_engine* uc, uint32_t irq)
{
  const uint64_t word = (uint64_t)k_nvic_iser_base + ((uint64_t)(irq / 32U) * 4U);
  const uint32_t iser = periph_rd32(uc, word);
  return ((iser >> (irq % 32U)) & 1U) != 0U;
}

/** @brief Mirror the pended line into NVIC ISPR so firmware can observe it. */
static void nvic_set_pending(uc_engine* uc, uint32_t irq)
{
  const uint64_t word = (uint64_t)k_nvic_ispr_base + ((uint64_t)(irq / 32U) * 4U);
  uint32_t       ispr = periph_rd32(uc, word);
  ispr |= (1U << (irq % 32U));
  (void)uc_mem_write(uc, word, &ispr, sizeof(ispr));
}

void board_periph_init(bool trace)
{
  s_trace = trace;
  for (uint32_t i = 0U; i < (uint32_t)k_port_count; i++) {
    s_port[i] = (port_state_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_agt_count; i++) {
    s_agt[i] = (agt_state_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_count; i++) {
    s_gpt[i] = (gpt_state_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    s_led_level[i]       = 0U;
    s_led_transitions[i] = 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_icu_ielsr_cnt; i++) {
    s_ielsr[i] = 0U;
  }
  for (uint32_t i = 0U; i < (uint32_t)k_irq_track_max; i++) {
    s_irq_taken[i] = 0U;
  }
  s_irq_head  = 0U;
  s_irq_tail  = 0U;
  s_irq_total = 0U;
}

/* =============================================================================
 * GPIO / PORT model.
 * =============================================================================
 */

/** @brief Note a board-LED edge when a traced port/pin output latch changes. */
static void port_trace_leds(uint32_t port_idx, uint16_t before, uint16_t after)
{
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    if (k_led_map[i].port != (uint8_t)port_idx) {
      continue;
    }
    const uint16_t mask = (uint16_t)(1U << k_led_map[i].pin);
    const uint32_t was  = ((before & mask) != 0U) ? 1U : 0U;
    const uint32_t now  = ((after & mask) != 0U) ? 1U : 0U;
    if (was != now) {
      s_led_level[i] = now;
      s_led_transitions[i]++;
      if (s_trace) {
        (void)fprintf(stderr, "  [trace] %s -> %s\n", k_led_map[i].name, now ? "ON" : "OFF");
      }
    }
  }
}

/** @brief Apply a new PODR value to a port and trace any LED transition. */
static void port_set_podr(uint32_t port_idx, uint16_t new_podr)
{
  const uint16_t old = s_port[port_idx].podr;
  if (old != new_podr) {
    port_trace_leds(port_idx, old, new_podr);
    s_port[port_idx].podr = new_podr;
  }
}

/** @brief Dispatch a PORT register read; returns PCNTR value for the port. */
static uint64_t port_read(uint64_t addr)
{
  const uint32_t idx = (uint32_t)((addr - (uint64_t)k_port_base) / (uint64_t)k_port_stride);
  const uint64_t off = (addr - (uint64_t)k_port_base) % (uint64_t)k_port_stride;
  if (idx >= (uint32_t)k_port_count) {
    return 0U;
  }
  const port_state_t* p = &s_port[idx];
  if (off == (uint64_t)k_port_pcntr1) {
    return ((uint32_t)p->podr << (uint32_t)k_half_shift) | (uint32_t)p->pdr;
  }
  if (off == (uint64_t)k_port_pcntr2) {
    /* PIDR reads the live pin level: an output pin reads back its driven
     * latch, so a firmware "read what I drove" check observes the real state. */
    return (uint32_t)(p->podr & p->pdr);
  }
  return 0U; /* PCNTR3 is write-only; PCNTR4 unmodelled -> 0. */
}

/** @brief Dispatch a PORT register write (PCNTR1 direction/latch, PCNTR3 set/clear). */
static void port_write(uint64_t addr, uint32_t value)
{
  const uint32_t idx = (uint32_t)((addr - (uint64_t)k_port_base) / (uint64_t)k_port_stride);
  const uint64_t off = (addr - (uint64_t)k_port_base) % (uint64_t)k_port_stride;
  if (idx >= (uint32_t)k_port_count) {
    return;
  }
  if (off == (uint64_t)k_port_pcntr1) {
    s_port[idx].pdr = (uint16_t)(value & (uint32_t)k_half_mask);
    port_set_podr(idx, (uint16_t)((value >> (uint32_t)k_half_shift) & (uint32_t)k_half_mask));
  } else if (off == (uint64_t)k_port_pcntr3) {
    const uint16_t posr = (uint16_t)(value & (uint32_t)k_half_mask);
    const uint16_t porr = (uint16_t)((value >> (uint32_t)k_half_shift) & (uint32_t)k_half_mask);
    uint16_t       podr = s_port[idx].podr;
    podr |= posr;            /* atomic set  */
    podr &= (uint16_t)~porr; /* atomic clear */
    port_set_podr(idx, podr);
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

/**
 * @brief Raise a peripheral ELC event: latch IELSR.IR and pend the NVIC line.
 *
 * @details
 * Scans the ICU IELSR event-link table (owned by this model -- see ::s_ielsr)
 * for the slot whose IELS field equals @p event. On a match the slot's IR flag
 * is set (exactly as hardware latches it), and -- if the matching NVIC line
 * (line == slot index) is enabled in ISER -- the line is pended in NVIC ISPR
 * and queued for the engine to take as a real exception. An event with no
 * linked, enabled slot is dropped, just as on silicon.
 *
 * @param[in,out] uc    Unicorn engine (NVIC ISER/ISPR live in PPB RAM).
 * @param[in]     event ELC event number the peripheral is asserting.
 * @return Nothing.
 * @since 0.1.0
 */
static void icu_raise_event(uc_engine* uc, uint16_t event)
{
  for (uint32_t slot = 0U; slot < (uint32_t)k_icu_ielsr_cnt; slot++) {
    if ((s_ielsr[slot] & (uint32_t)k_ielsr_iels_mask) != (uint32_t)event) {
      continue;
    }
    s_ielsr[slot] |= (uint32_t)k_ielsr_ir_mask; /* latch IR (RW1C by handler) */
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

/** @brief Dispatch an ICU IELSR write; IR is RW1C, the IELS/DTCE bits are RW. */
static void icu_write(uint64_t addr, uint32_t value)
{
  const uint32_t slot = icu_ielsr_slot(addr);
  if (slot >= (uint32_t)k_icu_ielsr_cnt) {
    return;
  }
  /* IR (bit 16) is write-one-to-clear: a written 1 clears the latched flag, a
   * written 0 leaves it. Every other bit takes the written value. */
  const uint32_t ir_now  = s_ielsr[slot] & (uint32_t)k_ielsr_ir_mask;
  const uint32_t ir_keep = ((value & (uint32_t)k_ielsr_ir_mask) != 0U) ? 0U : ir_now;
  s_ielsr[slot]          = (value & ~(uint32_t)k_ielsr_ir_mask) | ir_keep;
}

/* =============================================================================
 * AGT timer model -- 16-bit reloading down-counter (ra_agt.c semantics).
 * =============================================================================
 */

/** @brief Dispatch an AGT register read for channel @p ch at byte offset @p off. */
static uint64_t agt_read(uint32_t ch, uint64_t off)
{
  const agt_state_t* a = &s_agt[ch];
  if (off == (uint64_t)k_agt_off_agt) {
    return a->counter;
  }
  if (off == (uint64_t)k_agt_off_cma) {
    return a->cmpa;
  }
  if (off == (uint64_t)k_agt_off_cmb) {
    return a->cmpb;
  }
  if (off == (uint64_t)k_agt_off_cr) {
    return a->cr;
  }
  if (off == (uint64_t)k_agt_off_mr1) {
    return a->mr1;
  }
  return 0U;
}

/** @brief Dispatch an AGT register write; AGTCR.TSTART arms / status is RW1C. */
static void agt_write(uint32_t ch, uint64_t off, uint32_t value)
{
  agt_state_t* a = &s_agt[ch];
  if (off == (uint64_t)k_agt_off_agt) {
    a->counter = (uint16_t)value; /* AGT write both reloads and seeds count */
    a->reload  = (uint16_t)value;
  } else if (off == (uint64_t)k_agt_off_cma) {
    a->cmpa = (uint16_t)value;
  } else if (off == (uint64_t)k_agt_off_cmb) {
    a->cmpb = (uint16_t)value;
  } else if (off == (uint64_t)k_agt_off_cr) {
    /* TUNDF/TCMAF/TCMBF are write-0-to-clear (ra_agt clears by writing 0);
     * TSTART is RW. TCSTF tracks TSTART. */
    const uint8_t status_keep =
      (uint8_t)(a->cr & (uint8_t)value & (uint8_t)(k_agtcr_tundf | k_agtcr_tcmaf | k_agtcr_tcmbf));
    const uint8_t start = (uint8_t)(value & (uint8_t)k_agtcr_tstart);
    a->cr = (uint8_t)(start | (start != 0U ? (uint8_t)k_agtcr_tcstf : 0U) | status_keep);
  } else if (off == (uint64_t)k_agt_off_mr1) {
    a->mr1 = (uint8_t)value;
  }
}

/** @brief Advance one running AGT channel by one chunk; raise events on wrap. */
static void agt_tick_channel(uc_engine* uc, uint32_t ch)
{
  agt_state_t* a = &s_agt[ch];
  if ((a->cr & (uint8_t)k_agtcr_tstart) == 0U) {
    return; /* stopped: counter holds */
  }
  const uint32_t step = (uint32_t)k_agt_step_per_chunk;
  if (a->counter > step) {
    a->counter = (uint16_t)(a->counter - step);
    return;
  }
  /* Underflow: reload and set TUNDF (and TCMAF if compare-match A is armed). */
  const uint32_t span    = (uint32_t)a->reload + 1U;
  const uint32_t deficit = step - a->counter;
  a->counter             = (uint16_t)(a->reload - ((deficit - 1U) % span));
  a->cr |= (uint8_t)k_agtcr_tundf;
  a->underflows++;
  if (ch == 0U) {
    icu_raise_event(uc, (uint16_t)k_event_agt0_int);
  }
}

/* =============================================================================
 * GPT timer model -- 32-bit saw up-counter (ra_gpt.c semantics).
 * =============================================================================
 */

/** @brief Dispatch a GPT register read for channel @p ch at byte offset @p off. */
static uint64_t gpt_read(uint32_t ch, uint64_t off)
{
  const gpt_state_t* g = &s_gpt[ch];
  if (off == (uint64_t)k_gpt_off_gtcnt) {
    return g->cnt;
  }
  if (off == (uint64_t)k_gpt_off_gtpr) {
    return g->period;
  }
  if (off == (uint64_t)k_gpt_off_gtcr) {
    return g->cr;
  }
  if (off == (uint64_t)k_gpt_off_gtst) {
    return g->st;
  }
  return 0U; /* GTWP / GTSTR / GTSTP / GTCLR read as 0 in this model */
}

/** @brief Dispatch a GPT register write; GTSTR/GTSTP gate the counter. */
static void gpt_write(uint32_t ch, uint64_t off, uint32_t value)
{
  gpt_state_t* g = &s_gpt[ch];
  if (off == (uint64_t)k_gpt_off_gtcnt) {
    g->cnt = value;
  } else if (off == (uint64_t)k_gpt_off_gtpr) {
    g->period = value;
  } else if (off == (uint64_t)k_gpt_off_gtcr) {
    g->cr = value; /* CST (bit0) starts / stops the count */
  } else if (off == (uint64_t)k_gpt_off_gtstr) {
    if ((value & 1U) != 0U) {
      g->cr |= (uint32_t)k_gtcr_cst;
    }
  } else if (off == (uint64_t)k_gpt_off_gtstp) {
    if ((value & 1U) != 0U) {
      g->cr &= ~(uint32_t)k_gtcr_cst;
    }
  } else if (off == (uint64_t)k_gpt_off_gtclr) {
    if ((value & 1U) != 0U) {
      g->cnt = 0U;
    }
  } else if (off == (uint64_t)k_gpt_off_gtst) {
    /* GTST bits are cleared by writing the value back with target bits zero
     * (ra_gpt_clear_status), so the model keeps only bits still set. */
    g->st &= value;
  }
}

/** @brief Advance one running GPT channel by one chunk; raise overflow events. */
static void gpt_tick_channel(uc_engine* uc, uint32_t ch)
{
  gpt_state_t* g = &s_gpt[ch];
  if ((g->cr & (uint32_t)k_gtcr_cst) == 0U) {
    return; /* stopped */
  }
  const uint32_t period = (g->period == 0U) ? (uint32_t)k_u16_max : g->period;
  const uint32_t step   = (uint32_t)k_gpt_step_per_chunk;
  if ((g->cnt + step) <= period) {
    g->cnt += step;
    return;
  }
  /* Overflow past GTPR in saw mode: wrap and set TCFPO. */
  g->cnt = (uint32_t)((g->cnt + step) - period - 1U);
  g->st |= (uint32_t)k_gtst_tcfpo;
  g->overflows++;
  if (ch == 0U) {
    icu_raise_event(uc, (uint16_t)k_event_gpt0_ovf);
  }
}

/* =============================================================================
 * MMIO dispatch -- route an address to its block, else report "not handled".
 * =============================================================================
 */

/** @brief True iff @p addr is inside [@p base, @p base + @p span). */
static bool in_range(uint64_t addr, uint64_t base, uint64_t span)
{
  return (addr >= base) && (addr < (base + span));
}

uint64_t board_periph_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled)
{
  (void)size;
  *handled = true;
  if (in_range(addr, (uint64_t)k_port_base, (uint64_t)k_port_span)) {
    return port_read(addr);
  }
  if (in_range(addr, (uint64_t)k_agt_base, (uint64_t)k_agt_span)) {
    const uint32_t ch = (uint32_t)((addr - (uint64_t)k_agt_base) / (uint64_t)k_agt_stride);
    return agt_read(ch, (addr - (uint64_t)k_agt_base) % (uint64_t)k_agt_stride);
  }
  if (in_range(addr, (uint64_t)k_gpt_base, (uint64_t)k_gpt_span)) {
    const uint32_t ch = (uint32_t)((addr - (uint64_t)k_gpt_base) / (uint64_t)k_gpt_stride);
    return gpt_read(ch, (addr - (uint64_t)k_gpt_base) % (uint64_t)k_gpt_stride);
  }
  if (icu_ielsr_slot(addr) < (uint32_t)k_icu_ielsr_cnt) {
    (void)uc;
    return icu_read(addr);
  }
  *handled = false;
  return 0U;
}

void board_periph_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled)
{
  (void)size;
  (void)uc;
  *handled = true;
  if (in_range(addr, (uint64_t)k_port_base, (uint64_t)k_port_span)) {
    port_write(addr, (uint32_t)value);
    return;
  }
  if (in_range(addr, (uint64_t)k_agt_base, (uint64_t)k_agt_span)) {
    const uint32_t ch = (uint32_t)((addr - (uint64_t)k_agt_base) / (uint64_t)k_agt_stride);
    agt_write(ch, (addr - (uint64_t)k_agt_base) % (uint64_t)k_agt_stride, (uint32_t)value);
    return;
  }
  if (in_range(addr, (uint64_t)k_gpt_base, (uint64_t)k_gpt_span)) {
    const uint32_t ch = (uint32_t)((addr - (uint64_t)k_gpt_base) / (uint64_t)k_gpt_stride);
    gpt_write(ch, (addr - (uint64_t)k_gpt_base) % (uint64_t)k_gpt_stride, (uint32_t)value);
    return;
  }
  if (icu_ielsr_slot(addr) < (uint32_t)k_icu_ielsr_cnt) {
    icu_write(addr, (uint32_t)value);
    return;
  }
  *handled = false;
}

void board_periph_tick(uc_engine* uc)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_agt_count; ch++) {
    agt_tick_channel(uc, ch);
  }
  for (uint32_t ch = 0U; ch < (uint32_t)k_gpt_count; ch++) {
    gpt_tick_channel(uc, ch);
  }
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

/* =============================================================================
 * End-of-run summary (LED transitions, timer totals, per-IRQ counts).
 * =============================================================================
 */

void board_periph_report(uc_engine* uc)
{
  (void)uc;
  (void)fprintf(stderr, "  GPIO LEDs     :");
  for (uint32_t i = 0U; i < (uint32_t)k_board_led_count; i++) {
    (void)fprintf(stderr,
                  " [%s %s x%u]",
                  k_led_map[i].name,
                  s_led_level[i] ? "ON" : "OFF",
                  s_led_transitions[i]);
  }
  (void)fprintf(stderr, "\n");

  for (uint32_t ch = 0U; ch < (uint32_t)k_agt_count; ch++) {
    if (s_agt[ch].underflows > 0U) {
      (void)fprintf(stderr,
                    "  AGT%u          : counter=0x%04X underflows=%u (running=%s)\n",
                    ch,
                    s_agt[ch].counter,
                    s_agt[ch].underflows,
                    (s_agt[ch].cr & (uint8_t)k_agtcr_tstart) ? "yes" : "no");
    }
  }
  for (uint32_t ch = 0U; ch < (uint32_t)k_gpt_count; ch++) {
    if (s_gpt[ch].overflows > 0U) {
      (void)fprintf(stderr,
                    "  GPT%u          : cnt=0x%08X period=0x%08X overflows=%u\n",
                    ch,
                    s_gpt[ch].cnt,
                    s_gpt[ch].period,
                    s_gpt[ch].overflows);
    }
  }

  if (s_irq_total > 0U) {
    (void)fprintf(stderr, "  NVIC IRQs     : %u taken  [", s_irq_total);
    bool first = true;
    for (uint32_t i = 0U; i < (uint32_t)k_irq_track_max; i++) {
      if (s_irq_taken[i] > 0U) {
        (void)fprintf(stderr, "%sIRQ%u x%u", first ? "" : " ", i, s_irq_taken[i]);
        first = false;
      }
    }
    (void)fprintf(stderr, "]\n");
  } else {
    (void)fprintf(stderr, "  NVIC IRQs     : 0 taken (no event-linked, enabled line fired)\n");
  }
}
