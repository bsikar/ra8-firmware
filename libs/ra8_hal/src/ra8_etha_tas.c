/**
 * @file ra8_etha_tas.c
 * @brief ETHA time-aware shaper (TAS / 802.1Qbv) flows -- HUM Ch 32.4.2
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The TAS half of the ETHA driver, split out of ra8_etha.c because it is
 * the only part of the block that is a multi-step hardware FLOW rather than
 * a set of independent register writes. Three of the manual's flows are
 * implemented here verbatim:
 *
 *  - TAS RAM reset, HUM Figure 32.8 (Ch 32.4.2.6 p 1667);
 *  - TAS setting, HUM Figure 32.11 (Ch 32.4.2.9 p 1675-1676), which itself
 *    contains the per-entry learn flow of Figure 32.14 (Ch 32.4.2.12
 *    p 1677);
 *  - TAS entry read, HUM Figure 32.15 (Ch 32.4.2.13 p 1678).
 *
 * What was here before returned ``k_ra8_ok`` while programming the wrong
 * things into the right registers (#539). ``EATASGL0.TASGAL[7:0]`` is the
 * TAS RAM ENTRY ADDRESS -- "Configures the address in which the TAS entry
 * is learned" (HUM Ch 32.3.5.13 p 1652) -- and it was receiving the gate
 * state, so every learn iteration targeted an address derived from a gate
 * bitmask and the entry index was never written at all. ``EATASGL1`` bit 28
 * is ``TASGSL``, the entry's gate state (Ch 32.3.5.14 p 1652), and it was
 * receiving an unrelated "cut-through" flag. The mandatory per-queue entry
 * counts (``EATASENCi``) were never written, and ``EATASGLR.GL`` -- which
 * hardware raises on every ``EATASGL1`` write and lowers when the learn
 * lands -- was never polled, so the writes raced each other.
 *
 * Nothing detected any of that, because all five shaper calls returned
 * success from argument validation alone and no caller ever read hardware
 * back. ::ra8_etha_read_tas_entry exists so that is no longer true.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_etha.h"
#include "ra8_etha_internal.h"
#include "ra8_etha_regs.h"
#include "ra8_hw_err.h"
#include "ra8_log.h"

/**
 * @var s_tag
 * @brief Logger tag used by every TAS call in this translation unit.
 *
 * @details TU-local read-only logger tag, matching the string the other two
 * ETHA translation units use so a log reader sees one subsystem.
 * @note Read-only after init; treat as immutable.
 * @warning Not safe to mutate.
 * @since 0.1.0
 */
static const char* s_tag = "ETHA";

/**
 * @enum ra8_etha_tas_poll_t
 * @brief Spin budgets for the two TAS flows that wait on hardware.
 *
 * @details
 * Both waits are for an on-chip RAM operation that completes in a handful
 * of ETHA clocks, so the budget only has to be large enough that a healthy
 * part never reaches it and small enough that a wedged part is reported
 * rather than hanging the caller forever.
 *
 * @invariant Every value is a loop-iteration count, not a time.
 */
typedef enum : uint32_t {
  k_ra8_etha_tas_learn_spins = 100000U, /**< EATASGLR.GL / EATASGRR.GR budget. */
  k_ra8_etha_tas_ram_spins   = 100000U, /**< EATASRIRM.TASRR budget.           */
} ra8_etha_tas_poll_t;

/**
 * @enum ra8_etha_tas_word_t
 * @brief Word-width mask used when splitting the 64-bit cycle start time.
 *
 * @details ``EATASCSTC0`` and ``EATASCSTC1`` together hold one 64-bit start
 * time as two 32-bit halves, so the split needs a named 32-bit mask rather
 * than a bare literal.
 *
 * @invariant The value is exactly the width of one MMIO word.
 */
typedef enum : uint32_t {
  k_ra8_etha_tas_word_mask = 0xFFFFFFFFUL, /**< All 32 bits of one register. */
} ra8_etha_tas_word_t;

/**
 * @brief Learn one entry into the TAS RAM and wait for it to land.
 *
 * @details HUM Figure 32.14 (Ch 32.4.2.12 "TAS Entry i Learn Flow" p 1677):
 * set ``EATASGL0`` to the entry address, write ``EATASGL1``, then read
 * ``EATASGLR`` until ``GL`` is 0. Writing ``EATASGL1`` is what raises
 * ``GL``, so the poll must follow that write and not precede it.
 *
 * @param[in] reg     MMIO pointer to the port's ETHA window.
 * @param[in] address TAS RAM entry address for this entry.
 * @param[in] entry   Gate state and gate time to store.
 *
 * @return ra8_err_t Result code.
 * @retval k_ra8_ok             Hardware lowered EATASGLR.GL.
 * @retval k_ra8_err_hw_timeout GL stayed high for the whole budget.
 *
 * @pre `reg` points at a valid ETHA register window.
 * @pre `entry->gate_time_ns` already checked against TASGTL[27:0].
 * @post On success the entry is resident at `address` in the TAS RAM.
 * @post On timeout no further entry is attempted by the caller.
 *
 * @note Not thread-safe; the caller serialises per port.
 * @see ra8_etha_set_tas_schedule
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_tas_learn_entry(volatile r_etha_regs_t*     reg,
                                          uint8_t                     address,
                                          const ra8_etha_tas_entry_t* entry)
{
  /* HUM Ch 32.3.5.13 "EATASGL0 : TAS Gate Learn Register 0" p 1652 */
  reg->EATASGL0 = (uint32_t)address & k_ra8_etha_mask_tas_gal;

  uint32_t gl1 = entry->gate_time_ns & k_ra8_etha_mask_tas_gtl;
  if (entry->gate_open) {
    gl1 |= (uint32_t)1U << (uint32_t)k_ra8_etha_eatasgl1_tasgsl_pos;
  }
  /* HUM Ch 32.3.5.14 "EATASGL1 : TAS Gate Learn Register 1" p 1652 */
  reg->EATASGL1 = gl1;

  /* HUM Ch 32.3.5.15 "EATASGLR : TAS Gate Learn Result Register" p 1653 */
  const ra8_err_t err =
    ra8_hw_wait_flag_clear32(&reg->EATASGLR,
                             (uint32_t)1U << (uint32_t)k_ra8_etha_eatasglr_gl_pos,
                             (uint32_t)k_ra8_etha_tas_learn_spins);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_tas: EATASGLR.GL never cleared");
  }
  return err;
}

/**
 * @brief Reject a schedule hardware could not hold before any of it is written.
 *
 * @details Validates the whole request up front so a rejected schedule
 * leaves the previously active one running: once the first ``EATASENCi``
 * write lands there is no way back. Checks the per-queue pointer contract,
 * the ``TASGTL[27:0]`` width of every gate time, and the TAS RAM capacity
 * limit from HUM Ch 32.3.5.3 (p 1647).
 *
 * @param[in] queues Per-queue gate lists, ::k_ra8_etha_tc_count of them.
 *
 * @return ra8_err_t Result code.
 * @retval k_ra8_ok              Schedule is programmable as given.
 * @retval k_ra8_err_null_ptr    A non-empty queue has a nullptr entry list.
 * @retval k_ra8_err_invalid_arg Capacity or gate-time width exceeded.
 *
 * @pre `queues` is non-null and has ::k_ra8_etha_tc_count elements.
 * @pre Every queue's `count` is its own entry-list length.
 * @post No hardware register is touched on any path.
 * @post The caller may program the schedule iff this returned k_ra8_ok.
 *
 * @note Pure; no side effects at all.
 * @see ra8_etha_set_tas_schedule
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_tas_validate(const ra8_etha_tas_queue_t* queues)
{
  uint32_t total = 0U;
  RA8_LOOP_BOUND(k_ra8_etha_tc_count);
  for (uint8_t queue = 0U; queue < (uint8_t)k_ra8_etha_tc_count; ++queue) {
    const uint16_t count = queues[queue].count;
    if (count == 0U) {
      continue;
    }
    RA8_CHECK_NULL_PTR(queues[queue].entries, s_tag, "etha_tas: queue entries null");
    RA8_LOOP_BOUND(k_ra8_etha_tas_entries_max);
    for (uint16_t index = 0U; index < count; ++index) {
      if (queues[queue].entries[index].gate_time_ns > k_ra8_etha_mask_tas_gtl) {
        ra8_log_error(s_tag, "etha_tas: gate time exceeds TASGTL[27:0]");
        return k_ra8_err_invalid_arg;
      }
    }
    total += (uint32_t)count;
  }
  if (total > (uint32_t)k_ra8_etha_tas_entries_max) {
    ra8_log_error(s_tag, "etha_tas: total entries exceed TAS RAM capacity");
    return k_ra8_err_invalid_arg;
  }
  return k_ra8_ok;
}

/**
 * @brief Learn every queue's entries, laid out in queue order from `base`.
 *
 * @details The per-queue blocks are contiguous and start at the address
 * hardware nominated in ``EATASC.TASCA``, so queue 0's block begins at
 * `base` and each subsequent queue begins where the previous one ended.
 *
 * @param[in] reg    MMIO pointer to the port's ETHA window.
 * @param[in] queues Per-queue gate lists, ::k_ra8_etha_tc_count of them.
 * @param[in] base   TAS RAM address the first entry is written to.
 *
 * @return ra8_err_t Result code.
 * @retval k_ra8_ok             Every entry was learned.
 * @retval k_ra8_err_hw_timeout A learn never completed; nothing committed.
 *
 * @pre `queues` passed ::internal_tas_validate.
 * @pre `reg` points at a valid ETHA register window.
 * @post On success the TAS RAM holds every declared entry.
 * @post On failure the caller skips the commit.
 *
 * @note Not thread-safe; the caller serialises per port.
 * @see internal_tas_learn_entry
 * @since 0.1.0
 */
RA8_INTERNAL
static ra8_err_t internal_tas_learn_all(volatile r_etha_regs_t*     reg,
                                        const ra8_etha_tas_queue_t* queues,
                                        uint8_t                     base)
{
  uint32_t address = (uint32_t)base;
  RA8_LOOP_BOUND(k_ra8_etha_tc_count);
  for (uint8_t queue = 0U; queue < (uint8_t)k_ra8_etha_tc_count; ++queue) {
    RA8_LOOP_BOUND(k_ra8_etha_tas_entries_max);
    for (uint16_t index = 0U; index < queues[queue].count; ++index) {
      const ra8_err_t err = internal_tas_learn_entry(reg,
                                                     (uint8_t)(address & k_ra8_etha_mask_tas_gal),
                                                     &queues[queue].entries[index]);
      if (err != k_ra8_ok) {
        return err;
      }
      ++address;
    }
  }
  return k_ra8_ok;
}

ra8_err_t ra8_etha_tas_ram_reset(ra8_etha_port_t port)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "etha_tas_ram_reset: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra8_etha(port);

  /* HUM Ch 32.3.5.19 "EATASRIRM : TAS RAM Initialization Register Monitoring" p 1655 */
  reg->EATASRIRM = (uint32_t)1U << (uint32_t)k_ra8_etha_eatasrirm_tasriog_pos;

  /* HUM Ch 32.3.5.19 "EATASRIRM : TAS RAM Initialization Register Monitoring" p 1655 */
  const ra8_err_t err =
    ra8_hw_wait_flag_set32(&reg->EATASRIRM,
                           (uint32_t)1U << (uint32_t)k_ra8_etha_eatasrirm_tasrr_pos,
                           (uint32_t)k_ra8_etha_tas_ram_spins);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_tas_ram_reset: EATASRIRM.TASRR never asserted");
  }
  return err;
}

/**
 * @brief Write the schedule's sizing and timing registers.
 *
 * @details Steps 3 and 4 of HUM Figure 32.11: each queue's entry count to its
 * own ``EATASENCi``, the per-queue initial gate states to ``EATASIGSC``, and
 * the cycle start / cycle time to ``EATASCSTC0``, ``EATASCSTC1`` and
 * ``EATASCTC``. Split out of ::ra8_etha_set_tas_schedule so that function
 * stays inside the statement budget; it is one contiguous block of the
 * manual's flow with no decisions of its own.
 *
 * @param[in] reg                 MMIO pointer to the port's ETHA window.
 * @param[in] queues              Per-queue gate lists, one per traffic class.
 * @param[in] initial_gate_states EATASIGSC bitmap, bit q per queue q.
 * @param[in] cycle_time_ns       Value for EATASCTC.
 * @param[in] start_time          64-bit cycle start, split across CSTC0/CSTC1.
 *
 * @pre `queues` passed ::internal_tas_validate.
 * @pre `reg` points at a valid ETHA register window.
 * @post Every EATASENCi, EATASIGSC, EATASCSTC0/1 and EATASCTC is written.
 * @post No entry has been learned yet and nothing is committed.
 *
 * @note Not thread-safe; the caller serialises per port.
 * @see ra8_etha_set_tas_schedule
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_tas_program_timing(volatile r_etha_regs_t*     reg,
                                        const ra8_etha_tas_queue_t* queues,
                                        uint8_t                     initial_gate_states,
                                        uint32_t                    cycle_time_ns,
                                        uint64_t                    start_time)
{
  /* HUM Ch 32.3.5.2 "EATASIGSC : TAS Initial Gate State Configuration Register" p 1647 */
  reg->EATASIGSC = (uint32_t)initial_gate_states & k_ra8_etha_mask_tas_igs;
  RA8_LOOP_BOUND(k_ra8_etha_tc_count);
  for (uint8_t queue = 0U; queue < (uint8_t)k_ra8_etha_tc_count; ++queue) {
    /* HUM Ch 32.3.5.3 "EATASENCi : TAS Entry Number Configuration Register i" p 1647 */
    reg->EATASENC[queue] = (uint32_t)queues[queue].count & k_ra8_etha_mask_aen;
  }
  /* HUM Ch 32.3.5.7 "EATASCSTC0 : TAS Cycle Start Time Configuration Register 0" p 1649 */
  reg->EATASCSTC0 = (uint32_t)(start_time & (uint64_t)k_ra8_etha_tas_word_mask);
  /* HUM Ch 32.3.5.8 "EATASCSTC1 : TAS Cycle Start Time Configuration Register 1" p 1650 */
  reg->EATASCSTC1 = (uint32_t)((start_time >> 32U) & (uint64_t)k_ra8_etha_tas_word_mask);
  /* HUM Ch 32.3.5.11 "EATASCTC : TAS Cycle Time Configuration Register" p 1651 */
  reg->EATASCTC = cycle_time_ns;
}

ra8_err_t ra8_etha_set_tas_schedule(ra8_etha_port_t             port,
                                    const ra8_etha_tas_queue_t* queues,
                                    uint8_t                     initial_gate_states,
                                    uint32_t                    cycle_time_ns,
                                    uint64_t                    start_time)
{
  RA8_CHECK_NULL_PTR(queues, s_tag, "etha_set_tas_schedule: queues null");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "etha_set_tas_schedule: port out of range");
    return k_ra8_err_invalid_arg;
  }
  const ra8_err_t valid_err = internal_tas_validate(queues);
  if (valid_err != k_ra8_ok) {
    return valid_err;
  }

  volatile r_etha_regs_t* reg = ra8_etha(port);
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  const uint32_t tasc = reg->EATASC;
  if ((tasc & ((uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tasci_pos)) != 0U) {
    ra8_log_error(s_tag, "etha_set_tas_schedule: EATASC.TASCI set");
    return k_ra8_err_busy;
  }
  const uint8_t base =
    (uint8_t)((tasc >> (uint32_t)k_ra8_etha_eatasc_tasca_pos) & k_ra8_etha_mask_tas_gal);
  const bool already_enabled =
    (tasc & ((uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tase_pos)) != 0U;

  internal_tas_program_timing(reg, queues, initial_gate_states, cycle_time_ns, start_time);

  const ra8_err_t learn_err = internal_tas_learn_all(reg, queues, base);
  if (learn_err != k_ra8_ok) {
    return learn_err;
  }

  /* Commit: TASE always, TASCC only when this replaces a running schedule
   * (HUM Figure 32.11's two terminal boxes). */
  uint32_t commit = tasc | ((uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tase_pos);
  if (already_enabled) {
    commit |= (uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tascc_pos;
  } else {
    commit &= ~((uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tascc_pos);
  }
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  reg->EATASC = commit;
  return k_ra8_ok;
}

ra8_err_t ra8_etha_read_tas_entry(ra8_etha_port_t port, uint8_t address, ra8_etha_tas_entry_t* out)
{
  RA8_CHECK_NULL_PTR(out, s_tag, "etha_read_tas_entry: out null");
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "etha_read_tas_entry: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra8_etha(port);

  /* HUM Ch 32.3.5.16 "EATASGR : TAS Gate Read Register" p 1653 */
  reg->EATASGR = (uint32_t)address & k_ra8_etha_mask_tas_gal;

  /* HUM Ch 32.3.5.17 "EATASGRR : TAS Gate Read Result Register" p 1654 */
  const ra8_err_t err =
    ra8_hw_wait_flag_clear32(&reg->EATASGRR,
                             (uint32_t)1U << (uint32_t)k_ra8_etha_eatasgrr_gr_pos,
                             (uint32_t)k_ra8_etha_tas_learn_spins);
  if (err != k_ra8_ok) {
    ra8_log_error(s_tag, "etha_read_tas_entry: EATASGRR.GR never cleared");
    return err;
  }
  /* HUM Ch 32.3.5.17 "EATASGRR : TAS Gate Read Result Register" p 1654 */
  const uint32_t grr = reg->EATASGRR;
  out->gate_time_ns  = grr & k_ra8_etha_mask_tas_gtl;
  out->gate_open     = (grr & ((uint32_t)1U << (uint32_t)k_ra8_etha_eatasgrr_tasgsr_pos)) != 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_etha_enable_tas(ra8_etha_port_t port, uint8_t enable)
{
  if (!internal_port_ok(port)) {
    ra8_log_error(s_tag, "etha_enable_tas: port out of range");
    return k_ra8_err_invalid_arg;
  }
  volatile r_etha_regs_t* reg = ra8_etha(port);
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  const uint32_t tasc = reg->EATASC;
  const uint32_t bit  = (uint32_t)1U << (uint32_t)k_ra8_etha_eatasc_tase_pos;
  /* HUM Ch 32.3.5.1 "EATASC : TAS Configuration Register" p 1646 */
  reg->EATASC = (enable != 0U) ? (tasc | bit) : (tasc & ~bit);
  return k_ra8_ok;
}
