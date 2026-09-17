/**
 * @file board_periph_timer.c
 * @brief GPT + AGT timer peripheral-block model for the board emulator
 *
 * @details
 * Models the two RA8D2 general-purpose timer families with real, advancing
 * counters (ra8_gpt.c / ra8_agt.c semantics):
 *
 *  - **GPT** (ra8_gpt_regs.h): 14 channels of 32-bit saw up-counter. GTSTR /
 *    GTSTP / GTCR.CST gate the count, GTPR sets the period, and a wrap past the
 *    period sets GTST.TCFPO; GPT0 overflow raises the GPT0 ELC event.
 *  - **AGT** (ra8_agt_regs.h): 10 channels of 16-bit reloading down-counter.
 *    AGTCR.TSTART arms the count and an underflow reloads and sets AGTCR.TUNDF;
 *    AGT0 raises the AGT0 combined ELC event.
 *
 * The two families live in separate register windows, so this file registers a
 * descriptor for each with the board_periph core; the per-chunk advance and the
 * end-of-run report are attached to the AGT descriptor and cover BOTH families
 * (AGT channels then GPT channels) so the historical tick / report order is
 * preserved. Counter events are pended through the core's ICU -> NVIC path via
 * ::board_periph_icu_raise_event.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "emu_host_io_internal.h"

/** @brief AGT block geometry (ra8_agt_regs.h, 16-bit view). */
typedef enum : uint64_t {
  k_agt_base    = 0x40221000UL,   /**< AGT0 base.                    */
  k_agt_stride  = 0x100UL,        /**< Bytes per AGT channel.        */
  k_agt_count   = 10UL,           /**< AGT0..AGT9.                   */
  k_agt_span    = 0x100UL * 10UL, /**< AGT span.                     */
  k_agt_off_agt = 0x00UL,         /**< AGT counter (16-bit down).    */
  k_agt_off_cma = 0x02UL,         /**< AGTCMA compare-match A.       */
  k_agt_off_cmb = 0x04UL,         /**< AGTCMB compare-match B.       */
  k_agt_off_cr  = 0x08UL,         /**< AGTCR control/status (8-bit). */
  k_agt_off_mr1 = 0x09UL,         /**< AGTMR1 mode 1 (8-bit).        */
} agt_map_t;

/** @brief GPT block geometry (ra8_gpt_regs.h, 32-bit channels). */
typedef enum : uint64_t {
  k_gpt_base      = 0x40322000UL,   /**< GPT0 base.                 */
  k_gpt_stride    = 0x100UL,        /**< Bytes per GPT channel.     */
  k_gpt_count     = 14UL,           /**< GPT0..GPT13.               */
  k_gpt_span      = 0x100UL * 14UL, /**< GPT span.                  */
  k_gpt_off_gtstr = 0x04UL,         /**< GTSTR software start.      */
  k_gpt_off_gtstp = 0x08UL,         /**< GTSTP software stop.       */
  k_gpt_off_gtclr = 0x0CUL,         /**< GTCLR software clear.      */
  k_gpt_off_gtcr  = 0x2CUL,         /**< GTCR control (CST bit0).   */
  k_gpt_off_gtst  = 0x3CUL,         /**< GTST status.               */
  k_gpt_off_gtcnt = 0x48UL,         /**< GTCNT counter (32-bit up). */
  k_gpt_off_gtpr  = 0x64UL,         /**< GTPR period.               */
} gpt_map_t;

/** @brief 16-bit counter wrap value (also the GPT default period). */
typedef enum : uint32_t {
  k_u16_max = 0xFFFFU, /**< 16-bit counter wrap value. */
} timer_field_t;

/** @brief AGTCR (control/status) bits -- ra8_agt_agtcr_bits_t. */
typedef enum : uint32_t {
  k_agtcr_tstart = 0x01U, /**< TSTART start request.              */
  k_agtcr_tcstf  = 0x02U, /**< TCSTF count-status flag (RO).      */
  k_agtcr_tundf  = 0x20U, /**< TUNDF underflow flag (RW1C).       */
  k_agtcr_tcmaf  = 0x40U, /**< TCMAF compare-match A flag (RW1C). */
  k_agtcr_tcmbf  = 0x80U, /**< TCMBF compare-match B flag (RW1C). */
} agtcr_bit_t;

/** @brief GPT GTCR / GTST bits -- ra8_gpt register notes. */
typedef enum : uint32_t {
  k_gtcr_cst   = 0x00000001U, /**< GTCR.CST count-start.      */
  k_gtst_tcfa  = 0x00000001U, /**< GTST.TCFA compare-match A. */
  k_gtst_tcfpo = 0x00000040U, /**< GTST.TCFPO overflow.       */
  k_gtst_tcfpu = 0x00000080U, /**< GTST.TCFPU underflow.      */
} gpt_bit_t;

/**
 * @brief Canonical ELC event numbers the timer models emit (FSP ra8d2 bsp_elc).
 *
 * @details
 * RA8D2 ELC event signal table (HUM Ch 19): GPT0 overflow is 0x0C1 and AGT0
 * combined interrupt (underflow / compare-match) is 0x0DF. A firmware that
 * routes one of these through ra8_isr_register writes the same number into an
 * IELSR slot, so the ICU model can match the raised event to that slot.
 */
typedef enum : uint16_t {
  k_event_gpt0_ovf = 0x0C1U, /**< GPT0 GTCIV counter-overflow event.  */
  k_event_agt0_int = 0x0DFU, /**< AGT0 AGTI combined interrupt event. */
} elc_event_t;

/**
 * @brief Per-chunk advance for the modelled counters (one chunk == 1 tick).
 *
 * @details
 * The ra8_emulator run loop advances the timer counters and SysTick in lockstep --
 * one GPT/AGT step and one SysTick tick per emulation chunk. On silicon these
 * clocks are asynchronous: the GPT counts off PCLKD (megahertz) while a firmware
 * poll loop samples on the 1 kHz SysTick, so GTCNT never lands on the same value
 * at two consecutive samples. The model must preserve that "it is really
 * counting" observability.
 *
 * The GPT advance is therefore chosen ODD, i.e. coprime to the 2^N saw-PWM
 * periods the drivers use (GTPR = 0xFFFF -> 2^16 counts, GTPR = 0xFFFFFFFF ->
 * 2^32 counts). A power-of-two advance (the former @c 0x4000) evenly divides a
 * 2^16 period, so GTCNT visited only four distinct values and aliased to a
 * CONSTANT at any power-of-two sample interval -- e.g. gpt_pwm_demo samples
 * GTCNT every @c ra8_delay_ms(20) == 20 chunks, and @c 20 * 0x4000 == 5 * 0x10000
 * is an exact whole number of periods, so every sample read back the identical
 * count and the demo latched a false "timer wedged" mismatch. An odd advance is
 * coprime to 2^16 and 2^32, so @c n*step mod (period+1) has full period and no
 * fixed millisecond sample cadence can alias GTCNT to a constant. @c 0x4001 also
 * keeps the overflow cadence (~one wrap every four chunks) that the overflow-
 * driven demos (gpt_irq_demo, gpt_one_shot_demo) rely on.
 */
typedef enum : uint32_t {
  k_agt_step_per_chunk = 0x0800U,     /**< AGT down-count per chunk.               */
  k_gpt_step_per_chunk = 0x00004001U, /**< GPT up-count per chunk; odd (@details). */
} timer_tune_t;

/** @brief One AGT channel: a 16-bit reloading down-counter + status. */
typedef struct {
  uint16_t counter;    /**< Live AGT count.            */
  uint16_t reload;     /**< Value last written to AGT. */
  uint16_t cmpa;       /**< AGTCMA compare-match A.    */
  uint16_t cmpb;       /**< AGTCMB compare-match B.    */
  uint8_t  cr;         /**< AGTCR control/status.      */
  uint8_t  mr1;        /**< AGTMR1 mode 1.             */
  uint32_t underflows; /**< Underflow event count.     */
} agt_state_t;

/** @brief One GPT channel: a 32-bit saw up-counter + status. */
typedef struct {
  uint32_t cnt;       /**< GTCNT live count.     */
  uint32_t period;    /**< GTPR period.          */
  uint32_t cr;        /**< GTCR (CST in bit0).   */
  uint32_t st;        /**< GTST status flags.    */
  uint32_t overflows; /**< Overflow event count. */
} gpt_state_t;

static agt_state_t s_agt[k_agt_count];
static gpt_state_t s_gpt[k_gpt_count];

/* =============================================================================
 * AGT timer model -- 16-bit reloading down-counter (ra8_agt.c semantics).
 * =============================================================================
 */

/**
 * @brief Dispatch an AGT register read for channel @p ch at byte offset @p off.
 * @details Dispatch an agt register read for channel @p ch at byte offset @p off; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in] ch Selected channel identifier.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The agt reg read result produced by the board periph timer model.
 * @retval value The operation-specific agt reg read value.
 * @pre Arguments satisfy the ranges documented for agt reg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_agt_reg_read(uint32_t ch, uint64_t off)
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

/**
 * @brief Dispatch an AGT register write; AGTCR.TSTART arms / status is RW1C.
 * @details Dispatch an agt register write; agtcr.tstart arms / status is rw1c; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in] ch Selected channel identifier.
 * @param[in] off Register or byte offset addressed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for agt reg write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_agt_reg_write(uint32_t ch, uint64_t off, uint32_t value)
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
    /* TUNDF/TCMAF/TCMBF are write-0-to-clear (ra8_agt clears by writing 0);
     * TSTART is RW. TCSTF tracks TSTART. */
    const uint8_t status_keep =
      (uint8_t)(a->cr & (uint8_t)value & (uint8_t)(k_agtcr_tundf | k_agtcr_tcmaf | k_agtcr_tcmbf));
    const uint8_t start = (uint8_t)(value & (uint8_t)k_agtcr_tstart);
    a->cr = (uint8_t)(start | (start != 0U ? (uint8_t)k_agtcr_tcstf : 0U) | status_keep);
  } else if (off == (uint64_t)k_agt_off_mr1) {
    a->mr1 = (uint8_t)value;
  }
}

/**
 * @brief Advance one running AGT channel by one chunk; raise events on wrap.
 * @details Advance one running agt channel by one chunk; raise events on wrap; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] ch Selected channel identifier.
 * @pre Arguments satisfy the ranges documented for agt tick channel. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_agt_tick_channel(uc_engine* uc, uint32_t ch)
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
    board_periph_icu_raise_event(uc, (uint16_t)k_event_agt0_int);
  }
}

/**
 * @brief MMIO read inside the AGT window: route to the addressed channel.
 * @details MMIO read inside the agt window: route to the addressed channel; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The agt read result produced by the board periph timer model.
 * @retval value The operation-specific agt read value.
 * @pre Arguments satisfy the ranges documented for agt read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_agt_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_agt_base) / (uint64_t)k_agt_stride);
  return internal_agt_reg_read(ch, (addr - (uint64_t)k_agt_base) % (uint64_t)k_agt_stride);
}

/**
 * @brief MMIO write inside the AGT window: route to the addressed channel.
 * @details MMIO write inside the agt window: route to the addressed channel; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for agt write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_agt_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_agt_base) / (uint64_t)k_agt_stride);
  internal_agt_reg_write(ch,
                         (addr - (uint64_t)k_agt_base) % (uint64_t)k_agt_stride,
                         (uint32_t)value);
}

/* =============================================================================
 * GPT timer model -- 32-bit saw up-counter (ra8_gpt.c semantics).
 * =============================================================================
 */

/**
 * @brief Dispatch a GPT register read for channel @p ch at byte offset @p off.
 * @details Dispatch a gpt register read for channel @p ch at byte offset @p off; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in] ch Selected channel identifier.
 * @param[in] off Register or byte offset addressed by the operation.
 * @return The GPT reg read result produced by the board periph timer model.
 * @retval value The operation-specific GPT reg read value.
 * @pre Arguments satisfy the ranges documented for GPT reg read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_gpt_reg_read(uint32_t ch, uint64_t off)
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

/**
 * @brief Dispatch a GPT register write; GTSTR/GTSTP gate the counter.
 * @details Dispatch a gpt register write; gtstr/gtstp gate the counter; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in] ch Selected channel identifier.
 * @param[in] off Register or byte offset addressed by the operation.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for GPT reg write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_reg_write(uint32_t ch, uint64_t off, uint32_t value)
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
     * (ra8_gpt_clear_status), so the model keeps only bits still set. */
    g->st &= value;
  }
}

/**
 * @brief Advance one running GPT channel by one chunk; raise overflow events.
 * @details Advance one running gpt channel by one chunk; raise overflow events; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] ch Selected channel identifier.
 * @pre Arguments satisfy the ranges documented for GPT tick channel. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_gpt_tick_channel(uc_engine* uc, uint32_t ch)
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
    board_periph_icu_raise_event(uc, (uint16_t)k_event_gpt0_ovf);
  }
}

/**
 * @brief MMIO read inside the GPT window: route to the addressed channel.
 * @details MMIO read inside the gpt window: route to the addressed channel; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @return The GPT read result produced by the board periph timer model.
 * @retval value The operation-specific GPT read value.
 * @pre Arguments satisfy the ranges documented for GPT read. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static uint64_t internal_gpt_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_gpt_base) / (uint64_t)k_gpt_stride);
  return internal_gpt_reg_read(ch, (addr - (uint64_t)k_gpt_base) % (uint64_t)k_gpt_stride);
}

/**
 * @brief MMIO write inside the GPT window: route to the addressed channel.
 * @details MMIO write inside the gpt window: route to the addressed channel; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @param[in] addr Guest address involved in the operation.
 * @param[in] size Size of the requested region or access in bytes.
 * @param[in] value Register or payload value involved in the operation.
 * @pre Arguments satisfy the ranges documented for GPT write. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void
internal_gpt_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  (void)size;
  const uint32_t ch = (uint32_t)((addr - (uint64_t)k_gpt_base) / (uint64_t)k_gpt_stride);
  internal_gpt_reg_write(ch,
                         (addr - (uint64_t)k_gpt_base) % (uint64_t)k_gpt_stride,
                         (uint32_t)value);
}

/* =============================================================================
 * Shared tick / reset / report for both timer families (AGT then GPT).
 * =============================================================================
 */

/**
 * @brief Advance every running AGT channel, then every running GPT channel.
 * @details Advance every running agt channel, then every running gpt channel; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @param[in,out] uc Unicorn engine whose emulated state is read or updated.
 * @pre Arguments satisfy the ranges documented for timer tick. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_timer_tick(uc_engine* uc)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_agt_count; ch++) {
    internal_agt_tick_channel(uc, ch);
  }
  for (uint32_t ch = 0U; ch < (uint32_t)k_gpt_count; ch++) {
    internal_gpt_tick_channel(uc, ch);
  }
}

/**
 * @brief Clear all AGT + GPT channel state.
 * @details Clear all agt + gpt channel state; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for timer reset. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_timer_reset(void)
{
  for (uint32_t i = 0U; i < (uint32_t)k_agt_count; i++) {
    s_agt[i] = (agt_state_t){};
  }
  for (uint32_t i = 0U; i < (uint32_t)k_gpt_count; i++) {
    s_gpt[i] = (gpt_state_t){};
  }
}

/**
 * @brief Print one line per AGT / GPT channel that raised any event.
 * @details Print one line per agt / gpt channel that raised any event; this step is contained within the board periph timer model and uses bounded caller or module-owned storage.
 * @pre Arguments satisfy the ranges documented for timer report. @pre The call executes on the emulator's single owning thread.
 * @post State changes remain confined to the board periph timer model and documented output objects. @post Ownership of caller-supplied storage is unchanged.
 * @note The operation is synchronous and does not transfer heap ownership.
 * @since 0.1.0
 */
RA8_INTERNAL static void internal_timer_report(void)
{
  for (uint32_t ch = 0U; ch < (uint32_t)k_agt_count; ch++) {
    if (s_agt[ch].underflows > 0U) {
      (void)priv_emu_io_errf("  AGT%u          : counter=0x%04X underflows=%u (running=%s)\n",
                             ch,
                             s_agt[ch].counter,
                             s_agt[ch].underflows,
                             (s_agt[ch].cr & (uint8_t)k_agtcr_tstart) ? "yes" : "no");
    }
  }
  for (uint32_t ch = 0U; ch < (uint32_t)k_gpt_count; ch++) {
    if (s_gpt[ch].overflows > 0U) {
      (void)priv_emu_io_errf("  GPT%u          : cnt=0x%08X period=0x%08X overflows=%u\n",
                             ch,
                             s_gpt[ch].cnt,
                             s_gpt[ch].period,
                             s_gpt[ch].overflows);
    }
  }
}

/* The AGT descriptor carries the tick / reset / report that cover BOTH timer
 * families, so the historical AGT-then-GPT order is preserved; the GPT
 * descriptor only owns its own register window. Both share the timer tick
 * order, and registration order between them is irrelevant. */

/** @brief AGT register window + the combined timer tick / reset / report. */
static const board_periph_block_t s_k_agt_block = {
  .base   = (uint64_t)k_agt_base,
  .span   = (uint64_t)k_agt_span,
  .order  = (uint32_t)k_block_order_timer,
  .read   = internal_agt_read,
  .write  = internal_agt_write,
  .tick   = internal_timer_tick,
  .reset  = internal_timer_reset,
  .report = internal_timer_report,
  .name   = "AGT",
};

/** @brief GPT register window (tick / reset / report handled by the AGT block). */
static const board_periph_block_t s_k_gpt_block = {
  .base   = (uint64_t)k_gpt_base,
  .span   = (uint64_t)k_gpt_span,
  .order  = (uint32_t)k_block_order_timer,
  .read   = internal_gpt_read,
  .write  = internal_gpt_write,
  .tick   = nullptr,
  .reset  = nullptr,
  .report = nullptr,
  .name   = "GPT",
};

/** @brief Self-register both timer windows before main runs (decentralized). */
[[gnu::constructor]] RA8_INTERNAL static void internal_board_periph_timer_register(void)
{
  board_periph_register_block(&s_k_agt_block);
  board_periph_register_block(&s_k_gpt_block);
}
