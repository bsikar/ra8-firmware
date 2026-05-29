/**
 * @file ra_pdg.c
 * @brief PWM Delay Generation Circuit (PDG) driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Drives the PDG block per HUM Ch 23 (p 1152-1163). The driver
 * exposes init/deinit, per-edge delay programming, batch programming,
 * per-channel + per-pin power transition, status snapshot, runtime
 * FRANGE switch with proper DLL re-lock, GPT0/1/2/3 binding helpers,
 * compare-match-window constraint validation (HUM Ch 23.4.2 p 1162),
 * register-write-interval helpers (HUM Ch 23.4.3 p 1163), and an
 * event-callback hook.
 *
 * The PDG sits between GPT320..GPT323 and the GTIOC0..GTIOC3 pads
 * (HUM Figure 23.1 p 1153). Higher GPT channels (4..9) bypass the
 * PDG entirely and the driver therefore caps the channel index at
 * ``k_ra_pdg_channel_count`` = 4.
 *
 * @par State Machine
 * @startuml
 * [*] --> Stopped: module-stop bit asserted
 * Stopped --> Reset: ra_pdg_init unlocks MSTPD6
 * Reset --> Locking: DLLEN <- 1
 * Locking --> Running: 20 us elapsed (DLL locked)
 * Running --> Reset: ra_pdg_set_frange
 * Running --> Stopped: ra_pdg_deinit
 * @enduml
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#include "ra_pdg.h"

#include <stdint.h>

#include "ra8d2_gpt_regs.h"
#include "ra8d2_pdg_regs.h"
#include "ra_check.h"
#include "ra_err.h"
#include "ra_log.h"
#include "ra_mstp.h"

static const char* s_tag = "PDG";

/**
 * @enum ra_pdg_internal_t
 * @brief Driver-internal magic values (no magic numbers in functions).
 *
 * @details
 * - ``k_ra_pdg_channel_mask_all`` is a 4-bit mask covering channels
 * 0..3 -- used to validate the user's ``cfg->channel_mask``.
 * - ``k_ra_pdg_dll_lock_us`` matches the >= 20 us DLL-lock wait in
 * HUM Figure 23.2 p 1160. The host-test build collapses the spin
 * to a no-op via ``RA_SIMULATOR_MODE``.
 * - ``k_ra_pdg_busy_loops_per_us`` is a conservative lower bound on
 * the loop count needed to absorb one microsecond at the
 * Cortex-M85 1 GHz ceiling.
 */
typedef enum : uint16_t {
  k_ra_pdg_channel_mask_all  = 0x000FU, /**< Bits 0..3 -- 4 channels. */
  k_ra_pdg_dll_lock_us       = 20U,     /**< HUM Fig 23.2 wait. */
  k_ra_pdg_busy_loops_per_us = 1000U,   /**< Conservative spin floor. */
  k_ra_pdg_post_reset_loops  = 32U,     /**< >= 5 GTCLK cycles. */
  k_ra_pdg_ns_per_sec_kilo   = 1000U,   /**< Used in MHz->ns math. */
} ra_pdg_internal_t;

/**
 * @enum ra_pdg_gtwp_key_t
 * @brief GPT GTWP write-protect key values (mirrors FSP).
 *
 * @details
 * The upper byte holds the 0xA5 password; the LSB is the actual
 * WP bit. 0xA500 = unlock (WP cleared), 0xA501 = lock.
 */
/* HUM Ch 23.2.1 "Write protection via GPT320.GTWP.WP" p 1154 */
typedef enum : uint32_t {
  k_ra_pdg_gtwp_unlock = 0xA500U, /**< Unlock GTWP (WP = 0). */
  k_ra_pdg_gtwp_lock   = 0xA501U, /**< Re-lock GTWP (WP = 1). */
} ra_pdg_gtwp_key_t;

/**
 * @enum ra_pdg_constraint_t
 * @brief Numeric thresholds in HUM Ch 23.4.2 Table 23.4.
 */
/* HUM Ch 23.4.2 "Notes on Delay Settings for PWM Delay Generation Circuit" p 1162 */
typedef enum : uint32_t {
  k_ra_pdg_constraint_low_thresh = 2U, /**< "<= 2" boundary. */
  k_ra_pdg_constraint_high_pad   = 2U, /**< "GTPR - 2" boundary pad. */
} ra_pdg_constraint_t;

/**
 * @enum ra_pdg_write_interval_t
 * @brief Multipliers in the HUM Ch 23.4.3 (p 1163) interval formula.
 */
/* HUM Ch 23.4.3 "Register Write Interval" p 1163 */
typedef enum : uint32_t {
  k_ra_pdg_pclka_mult  = 6U, /**< Period_of_PCLKA x 6. */
  k_ra_pdg_gptclk_mult = 4U, /**< Period_of_GPTCLK x 4. */
} ra_pdg_write_interval_t;

/**
 * @enum ra_pdg_unit_t
 * @brief Units used by internal_freq_to_period_ns.
 */
typedef enum : uint32_t {
  k_ra_pdg_ns_per_sec = 1000000000UL, /**< 1 second in ns. */
} ra_pdg_unit_t;

/**
 * @brief Probe live PDG registers to decide whether ``ra_pdg_init``
 * has run successfully.
 *
 * @details
 * The PDG block is "initialized" when ``ra_pdg_init`` has driven the
 * DLL into the locked / running state, i.e. GTDLYCR.DLLEN == 1 and
 * GTDLYCR.DLYRST == 0 (HUM Ch 23.2.1 p 1154). Reading the register
 * directly avoids stale-static-flag bugs across host-test resets that
 * wipe the simulated MMIO without touching driver-side TU statics.
 *
 * @return ``true`` when DLLEN is asserted and DLYRST is cleared.
 *
 * @pre Module clock is reachable (caller did not unmap the PDG region).
 * @post No side effects.
 *
 * @note Re-entrant; reads two MMIO bits and derives a Boolean.
 *
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static bool internal_pdg_is_initialized(void)
{
  /* HUM Ch 23.2.1 "GTDLYCR : PWM Output Delay Control Register" p 1154 */
  const uint16_t cr = ra_pdg()->GTDLYCR;
  if ((cr & k_ra_pdg_gtdlycr_mask_dllen) == 0U) {
    return false;
  }
  if ((cr & k_ra_pdg_gtdlycr_mask_dlyrst) != 0U) {
    return false;
  }
  return true;
}

/**
 * @var s_pdg_event_fn
 * @brief Most recent callback registered by ``ra_pdg_attach_handler``.
 *
 * @warning Not protected against concurrent re-registration -- the
 * public API documents that the driver is single-threaded.
 */
static ra_pdg_event_fn_t s_pdg_event_fn;

/**
 * @var s_pdg_event_ctx
 * @brief Opaque pointer forwarded to ``s_pdg_event_fn``.
 */
static void* s_pdg_event_ctx;

/**
 * @brief Coarse software busy-wait used during init.
 *
 * @details
 * The HAL does not expose a microsecond busy-wait helper; on host
 * test builds the loop collapses to a no-op (``RA_SIMULATOR_MODE``)
 * because there is no timing requirement for the simulator.
 *
 * @param[in] usec Microseconds to spin.
 *
 * @pre ``usec`` >= 1.
 * @pre Caller has IRQs masked (otherwise the wait stretches).
 * @post Approximately ``usec`` microseconds have elapsed.
 *
 * @note Re-entrant; touches no globals or MMIO.
 *
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static void internal_busy_wait_us(uint16_t usec)
{
  for (uint16_t u = 0U; u < usec; ++u) {
    for (uint16_t i = 0U; i < k_ra_pdg_busy_loops_per_us; ++i) {
#ifndef RA_SIMULATOR_MODE
      __asm__ volatile("nop");
#endif
    }
  }
}

/**
 * @brief Spin >= 5 GTCLK cycles after releasing DLYRST (HUM Fig 23.2).
 *
 * @pre Caller has IRQs masked.
 * @post At least 5 GTCLK cycles have elapsed.
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static void internal_wait_5_gtclk(void)
{
  /* HUM Ch 23.3.1 "Adjustments to the Timing of Rising and Falling Edges" p 1159
 * The fastest GTCLK is 300 MHz, so 5 cycles ~= 17 ns; this loop
 * absorbs more than that on any reasonable build. */
  for (uint16_t i = 0U; i < k_ra_pdg_post_reset_loops; ++i) {
#ifndef RA_SIMULATOR_MODE
    __asm__ volatile("nop");
#endif
  }
}

/**
 * @brief Validate the FRANGE encoding.
 *
 * @return ``true`` if ``f`` is a legal FRANGE value (HUM 23.2.1 p 1154).
 *
 * @details See implementation.
 * @param[in] f See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static bool internal_frange_ok(ra_pdg_frange_t f)
{
  if (f == k_ra_pdg_frange_80_160_mhz) {
    return true;
  }
  if (f == k_ra_pdg_frange_155_300_mhz) {
    return true;
  }
  return false;
}

/**
 * @brief Validate a user-supplied PDG configuration.
 *
 * @param[in] cfg Caller-supplied descriptor (must be non-null).
 *
 * @return ``k_ra_ok`` if the FRANGE / channel_mask combination is
 * legal, else ``k_ra_err_invalid_arg`` /
 * ``k_ra_err_out_of_range``.
 *
 * @pre ``cfg`` is non-null.
 * @pre ``cfg`` lives in readable memory.
 * @post No side effects.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t internal_validate_cfg(const ra_pdg_config_t* cfg)
{
  if (((uint16_t)cfg->channel_mask & ~k_ra_pdg_channel_mask_all) != 0U) {
    return k_ra_err_invalid_arg;
  }
  if (cfg->auto_tune != 0U) {
    if (cfg->gptclk_hz == 0U) {
      return k_ra_err_invalid_arg;
    }
    if ((cfg->gptclk_hz < k_ra_pdg_freq_low_min_hz) ||
        (cfg->gptclk_hz > k_ra_pdg_freq_high_max_hz)) {
      return k_ra_err_out_of_range;
    }
    return k_ra_ok;
  }
  if (!internal_frange_ok(cfg->frange)) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Compute a pointer to the right delay register cell.
 *
 * @details
 * Centralises the (channel, pin, edge) -> register pointer math so
 * the various write/read entry points share one decoder. Callers
 * MUST have already validated the indices.
 */
static volatile uint16_t* internal_dly_cell(uint8_t channel, ra_pdg_pin_t pin, ra_pdg_edge_t edge)
{
  /* HUM Ch 23.2.3 "GTDLYRnA: GTIOCnA Rising Output Delay Register" p 1156 */
  /* HUM Ch 23.2.6 "GTDLYFnB: GTIOCnB Falling Output Delay Register" p 1158 */
  volatile r_pdg_regs_t*     reg = ra_pdg();
  volatile r_pdg_dly_pair_t* base =
    (edge == k_ra_pdg_edge_rising) ? &reg->GTDLYR[channel] : &reg->GTDLYF[channel];
  return (pin == k_ra_pdg_pin_a) ? &base->A : &base->B;
}

/**
 * @brief Validate a (channel, pin, edge, code) tuple.
 *
 * @details See implementation.
 * @param[in] channel See implementation.
 * @param[in] pin See implementation.
 * @param[in] edge See implementation.
 * @param[in] code See implementation.
 * @return Result code.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static ra_err_t
internal_validate_slot(uint8_t channel, ra_pdg_pin_t pin, ra_pdg_edge_t edge, uint8_t code)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((pin != k_ra_pdg_pin_a) && (pin != k_ra_pdg_pin_b)) {
    return k_ra_err_invalid_arg;
  }
  if ((edge != k_ra_pdg_edge_rising) && (edge != k_ra_pdg_edge_falling)) {
    return k_ra_err_invalid_arg;
  }
  if ((uint16_t)code > k_ra_pdg_dly_max) {
    return k_ra_err_invalid_arg;
  }
  return k_ra_ok;
}

/**
 * @brief Convert a frequency in Hz to a period in nanoseconds.
 *
 * @param[in] hz Frequency in Hz, must be > 0.
 * @return Period in ns.
 *
 * @details See implementation.
 * @retval k_ra_ok Operation succeeded.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
static uint32_t internal_freq_to_period_ns(uint32_t hz)
{
  /* hz > 0 guaranteed by callers. */
  return k_ra_pdg_ns_per_sec / hz;
}

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Run the six-step DLL bring-up sequence from HUM Fig 23.2.
 *
 * @details
 * HUM Ch 23.3.1 "Adjustments to the Timing of Rising and Falling Edges"
 * p 1159 describes the exact sequence: latch FRANGE, enable DLL, wait
 * >= 20 us for lock, release DLYRST, wait >= 5 GTCLK cycles, then turn
 * off bypass for the channels selected by ``cfg->channel_mask``.
 *
 * @param[in] cfg        Validated config.
 * @param[in] frange_use Final FRANGE band (post auto-tune resolution).
 *
 * @pre Module clock is ungated (caller has already taken care of MSTP).
 * @post DLL is locked, requested channels active, others in bypass.
 *
 * @note Internal helper, not thread-safe.
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @since 0.1.0
 */
static void internal_program_dll(const ra_pdg_config_t* cfg, ra_pdg_frange_t frange_use)
{
  volatile r_pdg_regs_t* reg = ra_pdg();

  /* Step 1 of Figure 23.2 (p 1160): DLLEN=0, DLYRST=1, DLYBSn=0,
   * FRANGE=user-supplied band. Performed in one 16-bit write so the
   * intermediate state is never visible. */
  /* HUM Ch 23.2.1 "GTDLYCR: PWM Output Delay Control Register" p 1154 */
  const uint16_t frange_bits =
    (uint16_t)((uint16_t)frange_use << (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  reg->GTDLYCR = (uint16_t)(k_ra_pdg_gtdlycr_mask_dlyrst | frange_bits);
  /* HUM Ch 23.2.2 "GTDLYCR2: PWM Output Delay Control Register 2" p 1155 */
  reg->GTDLYCR2 = 0U;

  /* Step 2: enable the DLL. */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR =
    (uint16_t)(k_ra_pdg_gtdlycr_mask_dlyrst | k_ra_pdg_gtdlycr_mask_dllen | frange_bits);

  /* Step 3: wait >= 20 us for the DLL to lock. */
  /* HUM Ch 23.3.1 "Adjustments to the Timing of Rising and Falling Edges" p 1159 */
  internal_busy_wait_us(k_ra_pdg_dll_lock_us);

  /* Step 4: release the PDG from reset. */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR = (uint16_t)(k_ra_pdg_gtdlycr_mask_dllen | frange_bits);

  /* Step 5: wait >= 5 GTCLK cycles. */
  internal_wait_5_gtclk();

  /* Step 6: turn off bypass for the channels actually in use. */
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  const uint16_t bypass_off =
    (uint16_t)((uint16_t)cfg->channel_mask << (uint16_t)k_ra_pdg_gtdlycr2_shift_dlybs);
  reg->GTDLYCR2 = bypass_off;
}

ra_err_t ra_pdg_init(const ra_pdg_config_t* cfg)
{
  RA_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  const ra_err_t cfg_err = internal_validate_cfg(cfg);
  RA_RETURN_ON_ERROR(cfg_err, s_tag, "pdg_init: cfg invalid"); /* GCOVR_EXCL_BR_LINE */

  ra_pdg_frange_t frange_use = cfg->frange;
  if (cfg->auto_tune != 0U) {
    const ra_err_t band_err = ra_pdg_pick_frange(cfg->gptclk_hz, &frange_use);
    RA_RETURN_ON_ERROR(band_err, s_tag, "pdg_init: auto-tune failed"); /* GCOVR_EXCL_BR_LINE */
  }

  /* HUM Ch 11.2.9 "MSTPCRD: Module Stop Control Register D" p 449 */
  const ra_err_t mst_err = ra_mstp_enable(k_ra_mstp_pdg);
  RA_RETURN_ON_ERROR(mst_err, s_tag, "pdg_init: mstp enable"); /* GCOVR_EXCL_BR_LINE */

  internal_program_dll(cfg, frange_use);

  ra_log_info(s_tag, "pdg_init");
  return k_ra_ok;
}

ra_err_t ra_pdg_deinit(void)
{
  volatile r_pdg_regs_t* reg = ra_pdg();
  /* Park the block: DLLEN=0, DLYRST=1, FRANGE=0. */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR = k_ra_pdg_gtdlycr_mask_dlyrst;
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  reg->GTDLYCR2 = 0U;

  /* Wipe every per-edge delay cell back to the post-reset value of 0. */
  /* HUM Ch 23.2.3 "GTDLYRnA" p 1156 */
  for (uint8_t ch = 0U; ch < k_ra_pdg_channel_count; ++ch) {
    reg->GTDLYR[ch].A = 0U;
    reg->GTDLYR[ch].B = 0U;
    /* HUM Ch 23.2.4 "GTDLYFnA" p 1156 */
    reg->GTDLYF[ch].A = 0U;
    reg->GTDLYF[ch].B = 0U;
  }

  /* PDG bit MSTPD6, see HUM Ch 23.4.1 p 1162. */
  /* HUM Ch 11.2.9 "MSTPCRD: Module Stop Control Register D" p 449 */
  (void)ra_mstp_disable(k_ra_mstp_pdg);
  return k_ra_ok;
}

/* =============================================================================
 * Per-edge delay programming
 * =============================================================================
 */

ra_err_t ra_pdg_set_delay(uint8_t channel, ra_pdg_pin_t pin, ra_pdg_edge_t edge, uint8_t code)
{
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  const ra_err_t v = internal_validate_slot(channel, pin, edge, code);
  RA_RETURN_ON_ERROR(v, s_tag, "set_delay: bad input"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 23.2.3 "GTDLYRnA: GTIOCnA Rising Output Delay Register" p 1156 */
  /* HUM Ch 23.2.4 "GTDLYFnA: GTIOCnA Falling Output Delay Register" p 1156 */
  /* HUM Ch 23.2.5 "GTDLYRnB: GTIOCnB Rising Output Delay Register" p 1157 */
  /* HUM Ch 23.2.6 "GTDLYFnB: GTIOCnB Falling Output Delay Register" p 1158 */
  volatile uint16_t* cell = internal_dly_cell(channel, pin, edge);
  *cell                   = (uint16_t)((uint16_t)code & k_ra_pdg_dly_mask);
  return k_ra_ok;
}

ra_err_t ra_pdg_get_delay(uint8_t channel, ra_pdg_pin_t pin, ra_pdg_edge_t edge, uint8_t* out_code)
{
  RA_CHECK_NULL_PTR(out_code, s_tag, "out_code must not be nullptr");
  const ra_err_t v = internal_validate_slot(channel, pin, edge, 0U);
  RA_RETURN_ON_ERROR(v, s_tag, "get_delay: bad input"); /* GCOVR_EXCL_BR_LINE */

  /* HUM Ch 23.2.3 "GTDLYRnA" p 1156 */
  /* HUM Ch 23.2.5 "GTDLYRnB" p 1157 */
  volatile uint16_t* cell = internal_dly_cell(channel, pin, edge);
  *out_code               = (uint8_t)(*cell & k_ra_pdg_dly_mask);
  return k_ra_ok;
}

ra_err_t ra_pdg_set_delay_batch(const ra_pdg_delay_entry_t* entries, uint8_t count)
{
  RA_CHECK_NULL_PTR(entries, s_tag, "entries must not be nullptr");
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  if ((count == 0U) || (count > k_ra_pdg_slot_count)) {
    return k_ra_err_invalid_arg;
  }
  /* Pre-validate every entry so a partial batch is impossible. */
  for (uint8_t i = 0U; i < count; ++i) {
    const ra_err_t v =
      internal_validate_slot(entries[i].channel, entries[i].pin, entries[i].edge, entries[i].code);
    if (v != k_ra_ok) {
      return v;
    }
  }
  /* HUM Ch 23.3.2 "Timing for Transfer of GTDLYRnA, GTDLYRnB, GTDLYFnA, GTDLYFnB
 * Register Settings" p 1161 -- temp-register transfer happens at next
 * overflow / underflow / trough; writing all entries back-to-back keeps the
 * batch coherent. */
  for (uint8_t i = 0U; i < count; ++i) {
    volatile uint16_t* cell =
      internal_dly_cell(entries[i].channel, entries[i].pin, entries[i].edge);
    *cell = (uint16_t)((uint16_t)entries[i].code & k_ra_pdg_dly_mask);
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_delay_ns_to_code(uint32_t        delay_ns,
                                 uint32_t        gptclk_hz,
                                 ra_pdg_frange_t frange,
                                 uint8_t*        out_code)
{
  RA_CHECK_NULL_PTR(out_code, s_tag, "out_code must not be nullptr");
  if (gptclk_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (!internal_frange_ok(frange)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.2.3 "GTDLYRnA: GTIOCnA Rising Output Delay Register" p 1156
 * code = round(delay_ns / (period_ns / divider))
 * = round(delay_ns * divider / period_ns)
 * = round(delay_ns * divider * gptclk_hz / 1e9) */
  const uint32_t divider = (frange == k_ra_pdg_frange_80_160_mhz) ? (uint32_t)k_ra_pdg_dly_div_low
                                                                  : (uint32_t)k_ra_pdg_dly_div_high;
  /* Use 64-bit arithmetic to avoid overflow. */
  const uint64_t numer = (uint64_t)delay_ns * (uint64_t)divider * (uint64_t)gptclk_hz;
  const uint64_t half  = (uint64_t)k_ra_pdg_ns_per_sec / 2U;
  uint64_t       code  = (numer + half) / (uint64_t)k_ra_pdg_ns_per_sec;
  if (code > (uint64_t)k_ra_pdg_dly_max) {
    code = (uint64_t)k_ra_pdg_dly_max;
  }
  *out_code = (uint8_t)code;
  return k_ra_ok;
}

/* =============================================================================
 * Per-channel and per-pin power transition
 * =============================================================================
 */

ra_err_t ra_pdg_exit_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.2.2 "GTDLYCR2: PWM Output Delay Control Register 2" p 1155
 * (DLYENn polarity is inverted: 0 = enabled, 1 = disabled). */
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         bit =
    (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlyen + (uint16_t)channel));
  reg->GTDLYCR2 = (uint16_t)(reg->GTDLYCR2 & (uint16_t)~bit);
  return k_ra_ok;
}

ra_err_t ra_pdg_enter_stop(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         bit =
    (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlyen + (uint16_t)channel));
  reg->GTDLYCR2 = (uint16_t)(reg->GTDLYCR2 | bit);
  return k_ra_ok;
}

ra_err_t ra_pdg_channel_bypass_set(uint8_t channel, uint8_t bypass)
{
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* DLYBSn = 1 means delay applied (bypass off). */
  /* HUM Ch 23.2.2 "GTDLYCR2: PWM Output Delay Control Register 2" p 1155 */
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         bit =
    (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlybs + (uint16_t)channel));
  if (bypass != 0U) {
    reg->GTDLYCR2 = (uint16_t)(reg->GTDLYCR2 | bit);
  } else {
    reg->GTDLYCR2 = (uint16_t)(reg->GTDLYCR2 & (uint16_t)~bit);
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_pin_disable(uint8_t channel, ra_pdg_pin_t pin)
{
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  if ((pin != k_ra_pdg_pin_a) && (pin != k_ra_pdg_pin_b)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.2.3 "GTDLYRnA" p 1156 + HUM Ch 23.2.4 "GTDLYFnA" p 1156 --
 * code 0x00 means "delay not applied" so writing 0 to both
 * rising and falling cells effectively disables the pin. */
  volatile uint16_t* cell_rise = internal_dly_cell(channel, pin, k_ra_pdg_edge_rising);
  volatile uint16_t* cell_fall = internal_dly_cell(channel, pin, k_ra_pdg_edge_falling);
  *cell_rise                   = 0U;
  *cell_fall                   = 0U;
  return k_ra_ok;
}

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

ra_err_t ra_pdg_get_status(uint16_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  volatile r_pdg_regs_t* reg = ra_pdg();
  *out                       = reg->GTDLYCR;
  return k_ra_ok;
}

ra_err_t ra_pdg_get_status_full(ra_pdg_status_full_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  volatile r_pdg_regs_t* reg = ra_pdg();
  const uint16_t         cr  = reg->GTDLYCR;
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  const uint16_t cr2 = reg->GTDLYCR2;
  out->raw_gtdlycr   = cr;
  out->raw_gtdlycr2  = cr2;
  out->dll_enabled   = (uint8_t)((cr & k_ra_pdg_gtdlycr_mask_dllen) != 0U);
  out->in_reset      = (uint8_t)((cr & k_ra_pdg_gtdlycr_mask_dlyrst) != 0U);
  out->frange        = (ra_pdg_frange_t)((cr & k_ra_pdg_gtdlycr_mask_frange) >>
                                         (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  for (uint8_t ch = 0U; ch < k_ra_pdg_channel_count; ++ch) {
    const uint16_t bs_bit = (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlybs + ch));
    const uint16_t en_bit = (uint16_t)(1U << ((uint16_t)k_ra_pdg_gtdlycr2_shift_dlyen + ch));
    out->per_channel_bypass_off[ch] = (uint8_t)((cr2 & bs_bit) != 0U);
    /* DLYENn is inverted: 0 = powered, 1 = power-off. */
    out->per_channel_powered[ch] = (uint8_t)((cr2 & en_bit) == 0U);
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_clear_status(uint16_t mask)
{
  /* Clearing DLYRST releases the block. */
  /* HUM Ch 23.2.1 "GTDLYCR: PWM Output Delay Control Register" p 1154 */
  volatile r_pdg_regs_t* reg = ra_pdg();
  if ((mask & k_ra_pdg_status_in_reset) != 0U) {
    reg->GTDLYCR = (uint16_t)(reg->GTDLYCR & (uint16_t)~k_ra_pdg_gtdlycr_mask_dlyrst);
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_attach_handler(ra_pdg_event_fn_t fn, void* ctx)
{
  s_pdg_event_fn  = fn;
  s_pdg_event_ctx = ctx;
  return k_ra_ok;
}

void ra_pdg_dispatch(void)
{
  const ra_pdg_event_fn_t fn  = s_pdg_event_fn;
  void* const             ctx = s_pdg_event_ctx;
  if (fn != nullptr) {
    fn(ctx);
  }
}

/* =============================================================================
 * Sweep 17: capture-style buffered delay-code apply + completion callback
 * =============================================================================
 */

/**
 * @var s_pdg_capture_in_flight
 * @brief Set true between `ra_pdg_capture_start` and the callback fire.
 *
 * @note Cleared by `ra_pdg_capture_stop` and after the callback fires.
 * @since 0.1.0
 */
static bool s_pdg_capture_in_flight;

ra_err_t ra_pdg_capture_start(const ra_pdg_delay_entry_t* buf, uint8_t len)
{
  RA_CHECK_NULL_PTR(buf, s_tag, "buf must not be nullptr");
  if (len == 0U) {
    return k_ra_err_invalid_arg;
  }
  const ra_err_t batch_err = ra_pdg_set_delay_batch(buf, len);
  if (batch_err != k_ra_ok) {
    return batch_err;
  }
  s_pdg_capture_in_flight = true;
  ra_pdg_dispatch();
  s_pdg_capture_in_flight = false;
  return k_ra_ok;
}

ra_err_t ra_pdg_capture_stop(void)
{
  s_pdg_capture_in_flight = false;
  return k_ra_ok;
}

/* =============================================================================
 * Runtime FRANGE switch + auto-tune calibration
 * =============================================================================
 */

ra_err_t ra_pdg_pick_frange(uint32_t gptclk_hz, ra_pdg_frange_t* out)
{
  RA_CHECK_NULL_PTR(out, s_tag, "out must not be nullptr");
  if (gptclk_hz == 0U) {
    return k_ra_err_invalid_arg;
  }
  if (gptclk_hz < k_ra_pdg_freq_low_min_hz) {
    return k_ra_err_out_of_range;
  }
  if (gptclk_hz > k_ra_pdg_freq_high_max_hz) {
    return k_ra_err_out_of_range;
  }
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 -- prefer the low band in the
 * 155..160 MHz overlap because it gives 1/128 (finer) resolution. */
  if (gptclk_hz <= k_ra_pdg_freq_overlap_top) {
    *out = k_ra_pdg_frange_80_160_mhz;
  } else {
    *out = k_ra_pdg_frange_155_300_mhz;
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_set_frange(ra_pdg_frange_t new_frange)
{
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  if (!internal_frange_ok(new_frange)) {
    return k_ra_err_invalid_arg;
  }

  volatile r_pdg_regs_t* reg = ra_pdg();
  /* Preserve the per-channel state across the FRANGE switch. */
  /* HUM Ch 23.2.2 "GTDLYCR2: PWM Output Delay Control Register 2" p 1155 */
  const uint16_t saved_cr2 = reg->GTDLYCR2;

  /* "Set the FRANGE[1:0] bits only when the DLLEN bit is 0." So drop
 * DLLEN and assert DLYRST first. */
  /* HUM Ch 23.2.1 "GTDLYCR: PWM Output Delay Control Register" p 1154 */
  reg->GTDLYCR = k_ra_pdg_gtdlycr_mask_dlyrst;

  /* Park GTDLYCR2 (Step 1 of Figure 23.2 also sets DLYBSn = 0). */
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  reg->GTDLYCR2 = 0U;

  const uint16_t frange_bits =
    (uint16_t)((uint16_t)new_frange << (uint16_t)k_ra_pdg_gtdlycr_shift_frange);
  /* Write FRANGE while DLLEN is still 0. */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR = (uint16_t)(k_ra_pdg_gtdlycr_mask_dlyrst | frange_bits);

  /* Re-run Steps 2..5 of HUM Figure 23.2 (p 1160). Enable DLL: */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR =
    (uint16_t)(k_ra_pdg_gtdlycr_mask_dlyrst | k_ra_pdg_gtdlycr_mask_dllen | frange_bits);
  /* 20 us DLL lock wait per HUM Figure 23.2. */
  /* HUM Ch 23.3.1 "Adjustments to the Timing of Rising and Falling Edges" p 1159 */
  internal_busy_wait_us(k_ra_pdg_dll_lock_us);
  /* Release reset. */
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 */
  reg->GTDLYCR = (uint16_t)(k_ra_pdg_gtdlycr_mask_dllen | frange_bits);
  internal_wait_5_gtclk();

  /* Restore the per-channel state. */
  /* HUM Ch 23.2.2 "GTDLYCR2" p 1155 */
  reg->GTDLYCR2 = saved_cr2;
  return k_ra_ok;
}

/* =============================================================================
 * GPT0/1/2/3 binding helpers
 * =============================================================================
 */

ra_err_t ra_pdg_bind_gpt_channel(uint8_t channel)
{
  if (!internal_pdg_is_initialized()) {
    return k_ra_err_not_initialized;
  }
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.2.3 "GTDLYRnA" p 1156 -- "GTDLYRnA can be written when
 * register write protection is disabled (GPT32n.GTWP.WP = 0)." */
  volatile r_gpt_channel_regs_t* gpt = ra_gpt(channel);
  if (gpt == nullptr) {
    return k_ra_err_hw_unmapped;
  }
  gpt->GTWP = k_ra_pdg_gtwp_unlock;
  /* HUM Ch 23.2.1 "GTDLYCR" p 1154 -- "be sure to read back the value of GTWP
 * before changing GTDLYCR". The dummy read enforces ordering. */
  (void)gpt->GTWP;
  return k_ra_ok;
}

ra_err_t ra_pdg_unbind_gpt_channel(uint8_t channel)
{
  if ((uint16_t)channel >= (uint16_t)k_ra_pdg_channel_count) {
    return k_ra_err_invalid_arg;
  }
  /* Restore write-protect on the host GPT channel. */
  /* HUM Ch 23.2.3 "GTDLYRnA: GTIOCnA Rising Output Delay Register" p 1156 */
  volatile r_gpt_channel_regs_t* gpt = ra_gpt(channel);
  if (gpt == nullptr) {
    return k_ra_err_hw_unmapped;
  }
  gpt->GTWP = k_ra_pdg_gtwp_lock;
  (void)gpt->GTWP;
  return k_ra_ok;
}

/* =============================================================================
 * Constraint validation (HUM Ch 23.4.2 / 23.4.3)
 * =============================================================================
 */

ra_err_t ra_pdg_check_constraints(ra_pdg_wave_mode_t mode,
                                  ra_pdg_count_dir_t dir,
                                  uint32_t           compare_match,
                                  uint32_t           gtpr)
{
  if ((mode != k_ra_pdg_wave_saw) && (mode != k_ra_pdg_wave_triangle)) {
    return k_ra_err_invalid_arg;
  }
  if ((dir != k_ra_pdg_dir_up) && (dir != k_ra_pdg_dir_down)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.4.2 "Notes on Delay Settings for PWM Delay Generation Circuit" p 1162 */
  if (mode == k_ra_pdg_wave_saw) {
    if (dir == k_ra_pdg_dir_up) {
      /* "Saw-wave Up: GTPR - 2 or above" -- compare_match must be < gtpr - 2. */
      if (gtpr < k_ra_pdg_constraint_high_pad) {
        /* gtpr is too small to even compute the boundary safely. */
        return k_ra_err_invalid_state;
      }
      const uint32_t boundary = gtpr - k_ra_pdg_constraint_high_pad;
      if (compare_match >= boundary) {
        return k_ra_err_invalid_state;
      }
    } else {
      /* "Saw-wave Down: <= 2". */
      if (compare_match <= k_ra_pdg_constraint_low_thresh) {
        return k_ra_err_invalid_state;
      }
    }
  } else {
    /* "Triangle-wave Down: <= 2". Up direction has no constraint per Table 23.4. */
    if (dir == k_ra_pdg_dir_down) {
      if (compare_match <= k_ra_pdg_constraint_low_thresh) {
        return k_ra_err_invalid_state;
      }
    }
  }
  return k_ra_ok;
}

ra_err_t ra_pdg_required_write_ns(uint32_t pclka_hz, uint32_t gptclk_hz, uint32_t* out_ns)
{
  RA_CHECK_NULL_PTR(out_ns, s_tag, "out_ns must not be nullptr");
  if ((pclka_hz == 0U) || (gptclk_hz == 0U)) {
    return k_ra_err_invalid_arg;
  }
  /* HUM Ch 23.4.3 "Register Write Interval" p 1163:
 * Write_Interval = Period_of_PCLKA x 6 + Period_of_GPTCLK x 4 */
  const uint32_t per_pclka  = internal_freq_to_period_ns(pclka_hz);
  const uint32_t per_gptclk = internal_freq_to_period_ns(gptclk_hz);
  *out_ns = (per_pclka * k_ra_pdg_pclka_mult) + (per_gptclk * k_ra_pdg_gptclk_mult);
  return k_ra_ok;
}
