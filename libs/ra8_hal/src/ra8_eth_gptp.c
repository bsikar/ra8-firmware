/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_eth_gptp.c
 * @brief Ethernet Generic PTP Timer driver implementation
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Implements the timer surface of HUM Ch 35 "Ethernet Generic PTP Timer
 * (GPTP)" p 1925-1964. Every register touched here appears in HUM
 * Table 35.3 p 1926 and carries the citation of its own subsection, not
 * the chapter-opening page.
 */

#include "ra8_eth_gptp.h"

#include <stdint.h>

#include "ra8_attributes.h"
#include "ra8_check.h"
#include "ra8_err.h"
#include "ra8_ether_regs.h"
#include "ra8_log.h"
#include "ra8_mstp.h"
#include "ra8_mstp_regs.h"

/**
 * @var s_tag
 * @brief Log tag for this driver.
 *
 * @details Passed to every ``ra8_log_*`` and ``RA8_CHECK_*`` call below.
 * @note Read-only after load; never reassigned.
 * @warning Do not modify: the HIL log scrapers key on this exact string.
 * @since 0.1.0
 */
static const char* s_tag = "ETHGPT";

/**
 * @var s_gptp_configured
 * @brief True once ::ra8_eth_gptp_init has programmed the block.
 *
 * @details
 * Every other entry point documents "``ra8_eth_gptp_init`` has released the
 * module-stop gate" as a precondition, and violating it is not a benign
 * mistake: the GPTP window is inside the ESWM domain, so a read or write
 * with MSTPC30 still asserted takes a bus fault. This flag turns that prose
 * precondition into an enforced one. It stays true across
 * ::ra8_eth_gptp_enter_stop / ::ra8_eth_gptp_exit_stop, because a module stop
 * is a clock gate rather than a reset and `PTPTIVCt` plus the 78-bit offset
 * survive it; only ::ra8_eth_gptp_deinit clears it.
 *
 * @note Not thread-safe; single-threaded init context assumed.
 * @warning Do not modify outside ::ra8_eth_gptp_init / ::ra8_eth_gptp_deinit.
 * @since 0.1.0
 */
static bool s_gptp_configured = false;

/**
 * @enum gptp_field_t
 * @brief Bit widths, shifts and masks of the GPTP timer registers.
 *
 * @details
 * `PTPTIVCt.TIV` is 5.27 fixed point (HUM 35.3.2.3 p 1928): the top five
 * bits are whole nanoseconds and the low 27 are the sub-nanosecond part,
 * so the sub-nanosecond scale is 2^27. The GPTP time is 78 bits with the
 * nanosecond part in bits [29:0] and the seconds split 32 + 16 across the
 * M and U registers (HUM 35.3.3.3 to 35.3.3.5 p 1931-1932).
 *
 * @invariant ::k_gptp_mask_nsec covers exactly 30 bits and
 *            ::k_gptp_mask_sec_u exactly 16.
 *
 * @par Example:
 * @code
 * const uint32_t nsec = reg->PTPGPTPTML & (uint32_t)k_gptp_mask_nsec;
 * @endcode
 *
 * @see ra8_eth_gptp_get_time
 */
typedef enum : uint32_t {
  k_gptp_subns_shift = 27UL,         /**< 5.27 fixed-point scale of TIV.     */
  k_gptp_round_half  = 2UL,          /**< Round-to-nearest divisor for TIV.  */
  k_gptp_mask_nsec   = 0x3FFFFFFFUL, /**< Mask for GPTPL / TOVL [29:0].      */
  k_gptp_mask_sec_u  = 0x0000FFFFUL, /**< Mask for GPTPU / TOVU [15:0].      */
  k_gptp_shift_sec_u = 32UL,         /**< Seconds upper part starts at 2^32. */
} gptp_field_t;

/**
 * @brief Point at one timer unit's register block.
 *
 * @details Resolves `base + 0x0020 + 0x40 * timer` through ::r_gptp_regs_t.
 * @param[in] timer Timer index, already validated by the caller.
 * @return Volatile pointer to the timer's register block.
 * @retval &ra8_gptp()->TIMER[timer] Always -- ``timer`` is pre-validated by the
 *         caller, so the accessor has no failure path and never yields
 *         ``nullptr``.
 * @pre ``timer`` is below ::k_ra8_gptp_timer_count.
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @post The returned pointer is never ``nullptr``.
 * @post No register was read or written.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
RA8_HW_REGISTER_ACCESS
static inline volatile r_gptp_timer_regs_t* internal_timer(ra8_gptp_timer_idx_t timer)
{
  /* HUM Ch 35.2 "gPTP Register List" Table 35.3 p 1926 -- per-timer block at
   * offset 0x0020 + 0x40 x t. */
  return &ra8_gptp()->TIMER[timer];
}

/**
 * @brief Stop one timer unit without validating its index.
 *
 * @details Writes `PTPTMDC.TDq`, which clears `PTPTMEC.TEq` in hardware.
 * @param[in] timer Timer index, already validated by the caller.
 * @pre ``timer`` is below ::k_ra8_gptp_timer_count.
 * @pre The ESWM module-stop gate is released.
 * @post `PTPTMEC.TE[timer]` reads 0.
 * @post The unit's counters read 0.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stop(ra8_gptp_timer_idx_t timer)
{
  /* HUM Ch 35.3.2.2 "PTPTMDC : Timer Disable Configuration Register" p 1928 */
  ra8_gptp()->PTPTMDC = 1U << (uint32_t)timer;
}

/**
 * @brief Stop every timer unit in the block.
 *
 * @details Loops ::k_ra8_gptp_timer_count times over ::internal_stop.
 * @pre The ESWM module-stop gate is released.
 * @pre No consumer is mid-read of either unit.
 * @post `PTPTMEC.TE` reads 0 for every unit.
 * @post Every unit's counters read 0.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_stop_all(void)
{
  for (uint8_t t = 0U; t < (uint8_t)k_ra8_gptp_timer_count; ++t) {
    internal_stop((ra8_gptp_timer_idx_t)t);
  }
}

/**
 * @brief Write the 78-bit offset of one timer unit in the mandated order.
 *
 * @details
 * `PTPTOVCtU` then `PTPTOVCtM` then `PTPTOVCtL`; the L write is what
 * commits the offset (HUM 35.4.1.3.1 Figure 35.4 p 1944-1945).
 *
 * @param[in] timer Timer index, already validated by the caller.
 * @param[in] sec   Seconds part, already range-checked by the caller.
 * @param[in] nsec  Nanoseconds part, already range-checked by the caller.
 * @pre ``timer`` is below ::k_ra8_gptp_timer_count.
 * @pre ``sec`` fits in 48 bits and ``nsec`` is below one second.
 * @post The three offset registers hold the requested value.
 * @post The unit's reported time reflects the new offset.
 * @note Not thread-safe unless documented otherwise.
 * @since 0.1.0
 */
RA8_INTERNAL
static void internal_write_offset(ra8_gptp_timer_idx_t timer, uint64_t sec, uint32_t nsec)
{
  volatile r_gptp_timer_regs_t* reg = internal_timer(timer);
  /* HUM Ch 35.3.2.6 "PTPTOVCtU : Timer t Offset Value Configuration Register U"
   * p 1930 -- seconds [47:32]. */
  reg->PTPTOVCU = (uint32_t)(sec >> (uint64_t)k_gptp_shift_sec_u) & (uint32_t)k_gptp_mask_sec_u;
  /* HUM Ch 35.3.2.5 "PTPTOVCMt : Timer t Offset Value Configuration Register M"
   * p 1929 -- seconds [31:0]. */
  reg->PTPTOVCM = (uint32_t)sec;
  /* HUM Ch 35.3.2.4 "PTPTOVCtL : Timer t Offset Value Configuration Register L"
   * p 1929 -- nanoseconds; this write commits the whole 78-bit offset. */
  reg->PTPTOVCL = nsec & (uint32_t)k_gptp_mask_nsec;
}

ra8_err_t ra8_eth_gptp_tiv_from_hz(uint32_t clk_hz, uint32_t* out_tiv)
{
  RA8_CHECK_NULL_PTR(out_tiv, s_tag, "out_tiv must not be nullptr");
  if (clk_hz == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* TIV = round(1e9 ns * 2^27 / clk_hz) -- HUM Ch 35.3.2.3 p 1928. */
  const uint64_t scaled = (uint64_t)k_ra8_gptp_ns_per_sec << (uint64_t)k_gptp_subns_shift;
  const uint64_t half   = (uint64_t)clk_hz / (uint64_t)k_gptp_round_half;
  const uint64_t tiv    = (scaled + half) / (uint64_t)clk_hz;
  if (tiv > (uint64_t)UINT32_MAX) {
    return k_ra8_err_out_of_range;
  }
  *out_tiv = (uint32_t)tiv;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_init(const ra8_eth_gptp_cfg_t* cfg)
{
  RA8_CHECK_NULL_PTR(cfg, s_tag, "cfg must not be nullptr");
  uint32_t        tiv     = 0U;
  const ra8_err_t tiv_err = ra8_eth_gptp_tiv_from_hz(cfg->clk_hz, &tiv);
  RA8_RETURN_ON_ERROR(tiv_err, s_tag, "gptp_init: clk_hz");

  /* HUM Ch 11.2.8 "MSTPCRC : Module Stop Control Register C" p 446 -- MSTPC30
   * gates the Layer-3 Ethernet Switch domain that contains GPTP. */
  const ra8_err_t mst_err = ra8_mstp_enable(k_ra8_mstp_eswm);
  RA8_RETURN_ON_ERROR(mst_err, s_tag, "gptp_init: mstp enable");

  for (uint8_t t = 0U; t < (uint8_t)k_ra8_gptp_timer_count; ++t) {
    const ra8_gptp_timer_idx_t timer = (ra8_gptp_timer_idx_t)t;
    internal_stop(timer);
    /* HUM Ch 35.3.2.3 "PTPTIVCt : Timer t Increment Value Configuration
     * Register (t = 0, 1)" p 1928 */
    internal_timer(timer)->PTPTIVC = tiv;
    internal_write_offset(timer, 0ULL, 0U);
  }
  s_gptp_configured = true;
  ra8_log_info(s_tag, "gptp_init");
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_deinit(void)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  for (uint8_t t = 0U; t < (uint8_t)k_ra8_gptp_timer_count; ++t) {
    const ra8_gptp_timer_idx_t timer = (ra8_gptp_timer_idx_t)t;
    internal_stop(timer);
    /* HUM Ch 35.3.2.3 "PTPTIVCt : Timer t Increment Value Configuration
     * Register (t = 0, 1)" p 1928 -- restore the 0 reset value. */
    internal_timer(timer)->PTPTIVC = 0UL;
    internal_write_offset(timer, 0ULL, 0U);
  }
  s_gptp_configured = false;
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_gptp_ip_version(uint32_t* out_version)
{
  RA8_CHECK_NULL_PTR(out_version, s_tag, "out_version must not be nullptr");
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  /* HUM Ch 35.3.1.1 "PTPIPV : IP Version Register" p 1927 -- read-only, reset
   * value nonzero. */
  *out_version = ra8_gptp()->PTPIPV;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_timer_enable(ra8_gptp_timer_idx_t timer)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 35.3.2.1 "PTPTMEC : Timer Enable Configuration Register" p 1927 --
   * writing 1 to bit q sets TEq; other bits are unaffected. */
  ra8_gptp()->PTPTMEC = 1U << (uint32_t)timer;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_timer_disable(ra8_gptp_timer_idx_t timer)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  internal_stop(timer);
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_timer_is_enabled(ra8_gptp_timer_idx_t timer, bool* out_enabled)
{
  RA8_CHECK_NULL_PTR(out_enabled, s_tag, "out_enabled must not be nullptr");
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 35.3.2.1 "PTPTMEC : Timer Enable Configuration Register" p 1927 */
  const uint32_t tmec = ra8_gptp()->PTPTMEC;
  *out_enabled        = ((tmec >> (uint32_t)timer) & 1U) != 0U;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_set_increment(ra8_gptp_timer_idx_t timer, uint32_t tiv)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  if (tiv == 0U) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 35.3.2.3 "PTPTIVCt : Timer t Increment Value Configuration Register
   * (t = 0, 1)" p 1928 -- writable at any time (Table 35.4 p 1946-1947). */
  internal_timer(timer)->PTPTIVC = tiv;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_get_increment(ra8_gptp_timer_idx_t timer, uint32_t* out_tiv)
{
  RA8_CHECK_NULL_PTR(out_tiv, s_tag, "out_tiv must not be nullptr");
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  /* HUM Ch 35.3.2.3 "PTPTIVCt : Timer t Increment Value Configuration Register
   * (t = 0, 1)" p 1928 */
  *out_tiv = internal_timer(timer)->PTPTIVC;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_set_offset(ra8_gptp_timer_idx_t timer, uint64_t sec, uint32_t nsec)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  if (sec > (uint64_t)k_ra8_gptp_sec_max) {
    return k_ra8_err_invalid_arg;
  }
  if (nsec > (uint32_t)k_ra8_gptp_nsec_max) {
    return k_ra8_err_invalid_arg;
  }
  internal_write_offset(timer, sec, nsec);
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_get_time(ra8_gptp_timer_idx_t timer, uint64_t* out_sec, uint32_t* out_nsec)
{
  RA8_CHECK_NULL_PTR(out_sec, s_tag, "out_sec must not be nullptr");
  RA8_CHECK_NULL_PTR(out_nsec, s_tag, "out_nsec must not be nullptr");
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gptp_timer_regs_t* reg = internal_timer(timer);
  /* HUM Ch 35.4.1.3.4 "GPTP Timer t Reading Flow" Figure 35.7 p 1946 -- read L
   * first: that read latches M and U to the same instant. */
  const uint32_t nsec = reg->PTPGPTPTML & (uint32_t)k_gptp_mask_nsec;
  /* HUM Ch 35.3.3.4 "PTPGPTPTMtM : GPTP Timer t Monitoring Register M
   * (t = 0, 1)" p 1931 -- seconds [31:0]. */
  const uint64_t sec_lo = (uint64_t)reg->PTPGPTPTMM;
  /* HUM Ch 35.3.3.5 "PTPGPTPTMtU : GPTP Timer t Monitoring Register U
   * (t = 0, 1)" p 1932 -- seconds [47:32]. */
  const uint64_t sec_hi = (uint64_t)(reg->PTPGPTPTMU & (uint32_t)k_gptp_mask_sec_u);
  *out_sec              = (sec_hi << (uint64_t)k_gptp_shift_sec_u) | sec_lo;
  *out_nsec             = nsec;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_get_avtp_ns(ra8_gptp_timer_idx_t timer, uint64_t* out_ns)
{
  RA8_CHECK_NULL_PTR(out_ns, s_tag, "out_ns must not be nullptr");
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  if (timer >= k_ra8_gptp_timer_count) {
    return k_ra8_err_invalid_arg;
  }
  volatile r_gptp_timer_regs_t* reg = internal_timer(timer);
  /* HUM Ch 35.4.1.3.3 "64-bit AVTP Timer t Reading Flow" Figure 35.6
   * p 1945-1946 -- read L first: that read latches U. */
  const uint64_t lo = (uint64_t)reg->PTPAVTPTML;
  /* HUM Ch 35.3.3.2 "PTPAVTPTMtU : AVTP Timer t Monitoring Register U
   * (t = 0, 1)" p 1931 */
  const uint64_t hi = (uint64_t)reg->PTPAVTPTMU;
  *out_ns           = (hi << (uint64_t)k_gptp_shift_sec_u) | lo;
  return k_ra8_ok;
}

ra8_err_t ra8_eth_gptp_enter_stop(void)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  internal_stop_all();
  return ra8_mstp_disable(k_ra8_mstp_eswm);
}

ra8_err_t ra8_eth_gptp_exit_stop(void)
{
  RA8_VALIDATE_INIT(s_gptp_configured, s_tag, "ra8_eth_gptp_init has not run");
  return ra8_mstp_enable(k_ra8_mstp_eswm);
}
