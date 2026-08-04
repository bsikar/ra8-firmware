/**
 * @file board_periph_gptp.c
 * @brief Ethernet Generic PTP Timer (GPTP) model for ra8_emulator -- HUM Ch 35
 *
 * @details
 * Models the timer surface of the RA8D2 GPTP block (HUM Ch 35 "Ethernet
 * Generic PTP Timer (GPTP)" p 1925-1964) so the GENUINE ra8_eth_gptp driver --
 * and the apps that assert against it -- run headless. The block is a
 * **timer**: two independent free-running counters that advance one nanosecond
 * per nanosecond off the ESWM clock, plus an additive 78-bit offset. Before
 * this block existed the @c 0x403E_0000 window fell through to the sparse
 * config-reflect fallback -- a write-back RAM shadow, where a counter can never
 * advance -- so @c eth_gptp_timestamp_demo / @c eth_tsn_tas_demo could not have
 * their strongest assertion (the counter advances one second per second against
 * an independent SysTick time base) checked anywhere. This block makes that
 * assertion run.
 *
 * What is modelled faithfully (per timer unit t = 0, 1 at @c +0x0020 + 0x40*t):
 *
 *  - **PTPTMEC / PTPTMDC** (HUM 35.3.2.1 p 1927): writing 1 to bit q of PTPTMEC
 *    starts unit q; writing 1 to bit q of PTPTMDC stops (and clears) it. While a
 *    unit is stopped every one of its counters reads 0, exactly as the manual
 *    states, so a disabled timer reports time zero rather than a stale value.
 *  - **Free-running advance** (HUM 35.5.1.1 p 1951): each emulator tick the
 *    unit's accumulator advances by @c PTPTIVCt * clk_cycles_this_tick, the same
 *    @c gptpTimer = TimerOffset + FreeRunningTimerValue equation the manual
 *    gives. The @c clk is ESWCLK (HUM 9.10.23 "EtherSW Clock (ESWCLK)" p 398),
 *    which the CGC brings up to PLL1P/4 = 250 MHz; one emulator tick is one
 *    SysTick period (::k_gptp_tick_hz = 1 kHz), so the counter tracks SysTick to
 *    the nanosecond and the demos' 10 % window closes. The rate is derived from
 *    the firmware's own @c PTPTIVCt (5.27 fixed-point ns/clk, HUM 35.3.2.3
 *    p 1928): a wrong increment drifts the counter here just as it would on
 *    silicon -- the model does not paper over a mis-programmed timer.
 *  - **PTPTOVCtL/M/U** (HUM 35.3.2.4 p 1929): the 78-bit additive offset, 48-bit
 *    seconds + 30-bit nanoseconds, committed on the L write (HUM 35.4.1.3.1
 *    Figure 35.4 p 1944-1945).
 *  - **PTPGPTPTMtL/M/U** and **PTPAVTPTMtL/U**: reading the L register latches
 *    the M/U registers to the same instant (HUM 35.4.1.3.4 Figure 35.7 p 1946 /
 *    35.4.1.3.3 Figure 35.6 p 1945-1946), so a three-read sample is atomic. The
 *    AVTP view is the same instant flattened to nanoseconds modulo 2^64
 *    (HUM 35.5.1.2 p 1952).
 *  - **PTPIPV** (HUM 35.3.1.1 p 1927): the read-only, reset-nonzero IP-version
 *    word (0x0000_0003). The apps read it as their "is this really the GPTP
 *    block?" probe, so the model reproduces the nonzero reset value.
 *
 * Honest-model note: the media-clock capture / recovery (PTPMCCCm / PTPMCRCm),
 * media-clock pin mapping, cyclic compare (PTPCCCc), the pulse-output timer and
 * the security-config registers all need the MEDIA_IN / MEDIA_OUT / CYCLIC_COMP
 * pins the EK-RA8D2 board does not route, so -- exactly as ra8_eth_gptp itself
 * leaves them alone -- they are served by a flat config-reflect shadow inside
 * this window (read back what was written), never a faked action.
 *
 * Window: the whole @c [0x403E_0000, 0x403E_1000) GPTP aperture (HUM Ch 35.2
 * "gPTP Register List" Table 35.3 p 1926). The ESWM media-mux at @c 0x403E_1400
 * sits above it and stays in the sparse fallback.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#include <stdint.h>
#include <stdio.h>

#include "board_periph_block.h"
#include "ra8_ether_regs.h"

/**
 * @enum gptp_win_geom_t
 * @brief Absolute window the block owns.
 *
 * @details The full GPTP aperture of HUM Ch 35.2 Table 35.3 p 1926, base
 * ::k_ra8_gptp_base_addr for ::k_gptp_win_span bytes.
 */
typedef enum : uint64_t {
  k_gptp_win_base = (uint64_t)k_ra8_gptp_base_addr, /**< GPTP window base (0x403E_0000). */
  k_gptp_win_span = 0x1000UL,                       /**< Whole GPTP aperture length.     */
} gptp_win_geom_t;

/** @brief Per-tick order slot: right after the R-Switch cluster (58). */
typedef enum : uint32_t {
  k_gptp_block_order = 59U, /**< Tick / reset / report order (just after ETH). */
} gptp_order_t;

/**
 * @enum gptp_time_const_t
 * @brief Clock rate, fixed-point scale and field widths of the GPTP time.
 *
 * @details
 * ::k_gptp_eswclk_hz is ESWCLK = PLL1P / 4 = 250 MHz (HUM 9.10.23 p 398), the
 * @c clk that drives the free-running counter. One emulator tick is one SysTick
 * period, so ::k_gptp_clk_per_tick = ::k_gptp_eswclk_hz / ::k_gptp_tick_hz is
 * the number of @c clk cycles that elapse per tick. @c PTPTIVCt is 5.27
 * fixed-point nanoseconds (HUM 35.3.2.3 p 1928), so the sub-nanosecond scale is
 * @c 2^::k_gptp_subns_shift and the free-running accumulator carries a whole
 * second every @c 1e9 << ::k_gptp_subns_shift fixed-point units. The GPTP time
 * is 78 bits: nanoseconds in [29:0] and seconds split 32 + 16 across the M and U
 * registers (HUM 35.3.3.3 to 35.3.3.5 p 1931-1932).
 *
 * @invariant ::k_gptp_mask_nsec covers exactly 30 bits, ::k_gptp_mask_sec_u
 *            exactly 16.
 */
typedef enum : uint64_t {
  k_gptp_eswclk_hz    = 250000000ULL,      /**< ESWCLK = PLL1P/4 (HUM 9.10.23 p 398).  */
  k_gptp_tick_hz      = 1000ULL,           /**< Ticks/s: one tick = one SysTick 1 ms.  */
  k_gptp_clk_per_tick = 250000ULL,         /**< ESWCLK cycles per emulator tick.       */
  k_gptp_subns_shift  = 27ULL,             /**< 5.27 fixed-point scale of PTPTIVCt.    */
  k_gptp_ns_per_sec   = 1000000000ULL,     /**< Nanoseconds in one second.             */
  k_gptp_mask_nsec    = 0x3FFFFFFFULL,     /**< GPTP / offset nanoseconds [29:0].      */
  k_gptp_mask_sec_u   = 0x0000FFFFULL,     /**< Seconds upper half [47:32], 16 bits.   */
  k_gptp_mask_sec_48  = 0xFFFFFFFFFFFFULL, /**< 48-bit seconds field.                  */
  k_gptp_shift_sec_u  = 32ULL,             /**< Seconds upper part starts at 2^32.     */
  k_gptp_ipv_reset    = 0x00000003ULL,     /**< PTPIPV read-only reset (HUM 35.3.1.1). */
  k_gptp_hz_per_mhz   = 1000000ULL,        /**< Hz per MHz, for the report line.       */
} gptp_time_const_t;

/** @brief Byte / word access constants for the flat config shadow. */
typedef enum : uint32_t {
  k_gptp_reg32_bytes = 4U,    /**< One 32-bit register.   */
  k_gptp_byte_mask   = 0xFFU, /**< Low 8 bits of a value. */
  k_gptp_shift_byte  = 8U,    /**< One-byte shift.        */
} gptp_access_const_t;

/**
 * @struct gptp_timer_t
 * @brief Runtime state of one GPTP timer unit.
 *
 * @details
 * @c enabled tracks PTPTMEC.TEq. @c off_sec / @c off_nsec are the committed
 * 78-bit offset (set on the PTPTOVCtL write). @c acc_sec plus @c acc_fixed are
 * the free-running counter: @c acc_fixed is the sub-second part in 5.27
 * fixed-point nanoseconds (always below one second) and @c acc_sec the whole
 * seconds carried out of it, so the pair never overflows. @c latch_gptp_m /
 * @c latch_gptp_u / @c latch_avtp_u hold the M/U halves sampled when the
 * matching L register was last read, giving an atomic three-read view.
 */
typedef struct {
  bool     enabled;      /**< PTPTMEC.TEq: unit is counting.                */
  uint64_t off_sec;      /**< Committed offset seconds (48-bit).            */
  uint32_t off_nsec;     /**< Committed offset nanoseconds (below 1e9).     */
  uint64_t acc_sec;      /**< Free-running whole seconds.                   */
  uint64_t acc_fixed;    /**< Free-running sub-second, 5.27 fixed-point ns. */
  uint32_t latch_gptp_m; /**< PTPGPTPTMtM latched on the L read.            */
  uint32_t latch_gptp_u; /**< PTPGPTPTMtU latched on the L read.            */
  uint32_t latch_avtp_u; /**< PTPAVTPTMtU latched on the L read.            */
} gptp_timer_t;

/**
 * @struct gptp_state_t
 * @brief Whole-block model state.
 *
 * @details @c win is the flat register shadow (config-reflect for every
 * register not modelled specially, including PTPTIVCt and the offset registers,
 * which read back what was written). @c timer holds the two timer units'
 * derived state.
 */
typedef struct {
  uint8_t      win[k_gptp_win_span];          /**< Flat config-reflect shadow. */
  gptp_timer_t timer[k_ra8_gptp_timer_count]; /**< Per-unit derived state.     */
} gptp_state_t;

/** @brief Singleton GPTP model state. */
static gptp_state_t s_gptp;

/* ------------------------------------------------------------------------- */
/* Flat config-reflect shadow helpers. */
/* ------------------------------------------------------------------------- */

/** @brief Read @p size bytes little-endian from the config shadow at @p off. */
static uint64_t gptp_shadow_read(uint64_t off, unsigned size)
{
  uint64_t v = 0U;
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_gptp_win_span); ++i) {
    v |= (uint64_t)s_gptp.win[off + (uint64_t)i] << (k_gptp_shift_byte * i);
  }
  return v;
}

/** @brief Write @p size bytes little-endian into the config shadow at @p off. */
static void gptp_shadow_write(uint64_t off, unsigned size, uint64_t value)
{
  for (unsigned i = 0U; (i < size) && ((off + (uint64_t)i) < (uint64_t)k_gptp_win_span); ++i) {
    s_gptp.win[off + (uint64_t)i] = (uint8_t)(value >> (k_gptp_shift_byte * i));
  }
}

/** @brief Read a 32-bit shadow register at window offset @p off. */
static uint32_t gptp_shadow_u32(uint64_t off)
{
  return (uint32_t)gptp_shadow_read(off, k_gptp_reg32_bytes);
}

/* ------------------------------------------------------------------------- */
/* Per-timer offset decode. */
/* ------------------------------------------------------------------------- */

/**
 * @brief Resolve a window offset to a timer unit and its in-block offset.
 *
 * @param[in]  off       Window byte offset.
 * @param[out] out_timer Receives the unit index when @p off is inside a unit.
 * @param[out] out_inoff Receives the in-block offset when @p off is inside a unit.
 * @return true when @p off lands inside one of the two per-timer blocks.
 * @retval true  @p *out_timer / @p *out_inoff describe the addressed unit.
 * @retval false @p off is a global register or the reserved tail.
 * @pre Both out-pointers are non-null.
 * @post On false the out-params are untouched.
 * @post No model state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool gptp_decode_timer(uint64_t off, uint32_t* out_timer, uint64_t* out_inoff)
{
  if (off < (uint64_t)k_ra8_gptp_off_timer0) {
    return false;
  }
  const uint64_t rel   = off - (uint64_t)k_ra8_gptp_off_timer0;
  const uint32_t timer = (uint32_t)(rel / (uint64_t)k_ra8_gptp_timer_stride);
  if (timer >= (uint32_t)k_ra8_gptp_timer_count) {
    return false;
  }
  *out_timer = timer;
  *out_inoff = rel % (uint64_t)k_ra8_gptp_timer_stride;
  return true;
}

/* ------------------------------------------------------------------------- */
/* Free-running counter -> reported (seconds, nanoseconds). */
/* ------------------------------------------------------------------------- */

/**
 * @brief Compute one unit's current GPTP time (offset + free-running counter).
 *
 * @details Implements @c gptpTimer = TimerOffset + FreeRunningTimerValue
 * (HUM 35.5.1.1 p 1951). A stopped unit reads time zero (HUM 35.3.2.1 p 1927).
 *
 * @param[in]  t        Timer unit index (< ::k_ra8_gptp_timer_count).
 * @param[out] out_sec  Receives the 48-bit seconds field.
 * @param[out] out_nsec Receives the 30-bit nanoseconds field.
 * @return Nothing.
 * @pre @p t is a valid unit index and both out-pointers are non-null.
 * @pre The caller holds no lock (single-threaded run loop).
 * @post @p *out_sec is below 2^48 and @p *out_nsec below one second.
 * @post No model state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void gptp_time_now(uint32_t t, uint64_t* out_sec, uint32_t* out_nsec)
{
  const gptp_timer_t* u = &s_gptp.timer[t];
  if (!u->enabled) {
    /* HUM Ch 35.3.2.1 "PTPTMEC : Timer Enable Configuration Register" p 1927 --
     * a stopped unit's counters read 0. */
    *out_sec  = 0U;
    *out_nsec = 0U;
    return;
  }
  const uint32_t free_ns    = (uint32_t)(u->acc_fixed >> (uint64_t)k_gptp_subns_shift);
  uint64_t       total_nsec = (uint64_t)u->off_nsec + (uint64_t)free_ns;
  uint64_t       carry_sec  = 0U;
  if (total_nsec >= (uint64_t)k_gptp_ns_per_sec) {
    total_nsec -= (uint64_t)k_gptp_ns_per_sec;
    carry_sec = 1U;
  }
  const uint64_t total_sec = (u->off_sec + u->acc_sec + carry_sec) & (uint64_t)k_gptp_mask_sec_48;
  *out_sec                 = total_sec;
  *out_nsec                = (uint32_t)(total_nsec & (uint64_t)k_gptp_mask_nsec);
}

/**
 * @brief Advance one running unit's free-running accumulator by one tick.
 *
 * @details Adds @c PTPTIVCt * ::k_gptp_clk_per_tick fixed-point nanoseconds
 * (HUM 35.5.1.1 p 1951, increment HUM 35.3.2.3 p 1928), then carries any whole
 * seconds out of the sub-second accumulator so it never overflows.
 *
 * @param[in] t Timer unit index (< ::k_ra8_gptp_timer_count).
 * @return Nothing.
 * @pre @p t is a valid unit index.
 * @pre The unit's PTPTIVCt lives in the config shadow.
 * @post A stopped or zero-increment unit is left unchanged.
 * @post @c acc_fixed stays below one second in fixed point.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void gptp_tick_timer(uint32_t t)
{
  gptp_timer_t* u = &s_gptp.timer[t];
  if (!u->enabled) {
    return;
  }
  const uint64_t tiv_off = (uint64_t)k_ra8_gptp_off_timer0 +
                           ((uint64_t)t * (uint64_t)k_ra8_gptp_timer_stride) +
                           (uint64_t)k_ra8_gptp_off_t_ptptivc;
  /* HUM Ch 35.3.2.3 "PTPTIVCt : Timer t Increment Value Configuration Register
   * (t = 0, 1)" p 1928 -- 5.27 fixed-point nanoseconds per clk. */
  const uint64_t tiv = (uint64_t)gptp_shadow_u32(tiv_off);
  if (tiv == 0U) {
    return;
  }
  u->acc_fixed += tiv * (uint64_t)k_gptp_clk_per_tick;
  const uint64_t one_sec_fixed = (uint64_t)k_gptp_ns_per_sec << (uint64_t)k_gptp_subns_shift;
  const uint64_t whole         = u->acc_fixed / one_sec_fixed;
  u->acc_sec += whole;
  u->acc_fixed -= whole * one_sec_fixed;
}

/* ------------------------------------------------------------------------- */
/* Enable / disable / offset commit. */
/* ------------------------------------------------------------------------- */

/** @brief Apply a PTPTMEC write: start each unit whose bit is set. */
static void gptp_apply_tmec(uint32_t value)
{
  /* HUM Ch 35.3.2.1 "PTPTMEC : Timer Enable Configuration Register" p 1927 --
   * writing 1 to bit q sets TEq; other bits are unaffected (write-1-to-set). */
  for (uint32_t t = 0U; t < (uint32_t)k_ra8_gptp_timer_count; ++t) {
    if ((value & (1UL << t)) != 0U) {
      s_gptp.timer[t].enabled = true;
    }
  }
}

/** @brief Apply a PTPTMDC write: stop and clear each unit whose bit is set. */
static void gptp_apply_tmdc(uint32_t value)
{
  /* HUM Ch 35.3.2.2 "PTPTMDC : Timer Disable Configuration Register" p 1928 --
   * writing 1 to bit q clears TEq; a stopped unit's counters read 0, so a stop
   * is also a clear of the free-running accumulator. */
  for (uint32_t t = 0U; t < (uint32_t)k_ra8_gptp_timer_count; ++t) {
    if ((value & (1UL << t)) != 0U) {
      s_gptp.timer[t].enabled   = false;
      s_gptp.timer[t].acc_sec   = 0U;
      s_gptp.timer[t].acc_fixed = 0U;
    }
  }
}

/** @brief Read PTPTMEC back as the live enable bitmask across the units. */
static uint32_t gptp_read_tmec(void)
{
  uint32_t v = 0U;
  for (uint32_t t = 0U; t < (uint32_t)k_ra8_gptp_timer_count; ++t) {
    if (s_gptp.timer[t].enabled) {
      v |= (1UL << t);
    }
  }
  return v;
}

/**
 * @brief Commit one unit's 78-bit offset from the staged PTPTOVCt registers.
 *
 * @details The PTPTOVCtL write is what commits the whole offset
 * (HUM 35.4.1.3.1 Figure 35.4 p 1944-1945); PTPTOVCtU / PTPTOVCtM were written
 * first and already sit in the config shadow, so this reads them back and forms
 * @c {U[15:0], M[31:0]} seconds with @c L[29:0] nanoseconds.
 *
 * @param[in] t Timer unit index (< ::k_ra8_gptp_timer_count).
 * @return Nothing.
 * @pre @p t is a valid unit index.
 * @pre The three offset registers hold the intended value in the shadow.
 * @post @c off_sec is 48-bit and @c off_nsec below one second.
 * @post No counter state is modified.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void gptp_commit_offset(uint32_t t)
{
  const uint64_t base =
    (uint64_t)k_ra8_gptp_off_timer0 + ((uint64_t)t * (uint64_t)k_ra8_gptp_timer_stride);
  /* HUM Ch 35.3.2.6 "PTPTOVCtU : Timer t Offset Value Configuration Register U"
   * p 1930 -- seconds [47:32]. */
  const uint64_t ovcu = (uint64_t)gptp_shadow_u32(base + (uint64_t)k_ra8_gptp_off_t_ptptovcu);
  /* HUM Ch 35.3.2.5 "PTPTOVCMt : Timer t Offset Value Configuration Register M"
   * p 1929 -- seconds [31:0]. */
  const uint64_t ovcm = (uint64_t)gptp_shadow_u32(base + (uint64_t)k_ra8_gptp_off_t_ptptovcm);
  /* HUM Ch 35.3.2.4 "PTPTOVCtL : Timer t Offset Value Configuration Register L"
   * p 1929 -- nanoseconds [29:0]; this write commits the whole offset. */
  const uint64_t ovcl = (uint64_t)gptp_shadow_u32(base + (uint64_t)k_ra8_gptp_off_t_ptptovcl);
  s_gptp.timer[t].off_sec =
    ((ovcu & (uint64_t)k_gptp_mask_sec_u) << (uint64_t)k_gptp_shift_sec_u) | ovcm;
  s_gptp.timer[t].off_nsec = (uint32_t)(ovcl & (uint64_t)k_gptp_mask_nsec);
}

/* ------------------------------------------------------------------------- */
/* Monitoring reads (with the L-read M/U latch). */
/* ------------------------------------------------------------------------- */

/**
 * @brief Serve a read of a per-timer monitoring register, or report "not one".
 *
 * @details Reading PTPGPTPTMtL / PTPAVTPTMtL samples the counter and latches the
 * matching M/U halves (HUM 35.4.1.3.4 Figure 35.7 p 1946 / 35.4.1.3.3 Figure 35.6
 * p 1945-1946); reading M/U returns the latched half. The AVTP view is the same
 * instant flattened to nanoseconds modulo 2^64 (HUM 35.5.1.2 p 1952).
 *
 * @param[in]  t     Timer unit index (< ::k_ra8_gptp_timer_count).
 * @param[in]  inoff In-block offset inside the unit.
 * @param[out] out   Receives the register value when it is a monitoring register.
 * @return true when @p inoff addressed a monitoring register (@p *out is set).
 * @retval true  A monitoring register was served.
 * @retval false @p inoff is a config register (caller uses the shadow).
 * @pre @p t is a valid unit index and @p out is non-null.
 * @post On true the L reads refresh this unit's M/U latches.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static bool gptp_read_monitor(uint32_t t, uint64_t inoff, uint32_t* out)
{
  gptp_timer_t* u = &s_gptp.timer[t];
  if (inoff == (uint64_t)k_ra8_gptp_off_t_ptpgptptml) {
    uint64_t sec  = 0U;
    uint32_t nsec = 0U;
    gptp_time_now(t, &sec, &nsec);
    u->latch_gptp_m = (uint32_t)sec;
    u->latch_gptp_u =
      (uint32_t)((sec >> (uint64_t)k_gptp_shift_sec_u) & (uint64_t)k_gptp_mask_sec_u);
    *out = nsec;
    return true;
  }
  if (inoff == (uint64_t)k_ra8_gptp_off_t_ptpgptptmm) {
    *out = u->latch_gptp_m;
    return true;
  }
  if (inoff == (uint64_t)k_ra8_gptp_off_t_ptpgptptmu) {
    *out = u->latch_gptp_u;
    return true;
  }
  if (inoff == (uint64_t)k_ra8_gptp_off_t_ptpavtptml) {
    uint64_t sec  = 0U;
    uint32_t nsec = 0U;
    gptp_time_now(t, &sec, &nsec);
    const uint64_t avtp = (sec * (uint64_t)k_gptp_ns_per_sec) + (uint64_t)nsec;
    u->latch_avtp_u     = (uint32_t)(avtp >> (uint64_t)k_gptp_shift_sec_u);
    *out                = (uint32_t)avtp;
    return true;
  }
  if (inoff == (uint64_t)k_ra8_gptp_off_t_ptpavtptmu) {
    *out = u->latch_avtp_u;
    return true;
  }
  return false;
}

/* ------------------------------------------------------------------------- */
/* MMIO dispatch. */
/* ------------------------------------------------------------------------- */

/**
 * @brief MMIO read handler for the GPTP window.
 *
 * @param[in,out] uc   Active engine (unused: GPTP reads touch no SRAM).
 * @param[in]     addr Absolute register address inside the window.
 * @param[in]     size Access width (1 / 2 / 4).
 * @return The register value.
 * @retval value A modelled register or the config shadow.
 * @pre @p addr is inside @c [k_gptp_win_base, k_gptp_win_base + k_gptp_win_span).
 * @pre @p size is 1, 2, or 4.
 * @post No emulated memory is modified.
 * @post An L monitoring read refreshes the addressed unit's M/U latches.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static uint64_t gptp_read(uc_engine* uc, uint64_t addr, unsigned size)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_gptp_win_base;
  if (off == (uint64_t)k_ra8_gptp_off_ptpipv) {
    /* HUM Ch 35.3.1.1 "PTPIPV : IP Version Register" p 1927 -- read-only, reset
     * value nonzero; the apps use it as the block-presence probe. */
    return (uint64_t)k_gptp_ipv_reset;
  }
  if (off == (uint64_t)k_ra8_gptp_off_ptptmec) {
    return (uint64_t)gptp_read_tmec();
  }
  if (off == (uint64_t)k_ra8_gptp_off_ptptmdc) {
    /* HUM Ch 35.3.2.2 "PTPTMDC : Timer Disable Configuration Register" p 1928 --
     * write-only; reads back 0. */
    return 0U;
  }
  uint32_t timer = 0U;
  uint64_t inoff = 0U;
  if (gptp_decode_timer(off, &timer, &inoff)) {
    uint32_t val = 0U;
    if (gptp_read_monitor(timer, inoff, &val)) {
      return (uint64_t)val;
    }
  }
  return gptp_shadow_read(off, size);
}

/**
 * @brief MMIO write handler for the GPTP window.
 *
 * @param[in,out] uc    Active engine (unused: GPTP writes touch no SRAM).
 * @param[in]     addr  Absolute register address inside the window.
 * @param[in]     size  Access width (1 / 2 / 4).
 * @param[in]     value Value written.
 * @return Nothing.
 * @pre @p addr is inside the block window.
 * @pre @p size is 1, 2, or 4.
 * @post PTPTMEC / PTPTMDC drive the enable state; a PTPTOVCtL write commits the
 *       offset; every other register reflects @p value in the shadow.
 * @note Not thread-safe.
 * @since 0.1.0
 */
static void gptp_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value)
{
  (void)uc;
  const uint64_t off = addr - (uint64_t)k_gptp_win_base;
  if (off == (uint64_t)k_ra8_gptp_off_ptptmec) {
    gptp_apply_tmec((uint32_t)value);
    return;
  }
  if (off == (uint64_t)k_ra8_gptp_off_ptptmdc) {
    gptp_apply_tmdc((uint32_t)value);
    return;
  }
  if (off == (uint64_t)k_ra8_gptp_off_ptpipv) {
    return; /* PTPIPV is read-only (HUM 35.3.1.1 p 1927) -- drop the write. */
  }
  gptp_shadow_write(off, size, value);
  uint32_t timer = 0U;
  uint64_t inoff = 0U;
  if (gptp_decode_timer(off, &timer, &inoff) && (inoff == (uint64_t)k_ra8_gptp_off_t_ptptovcl)) {
    gptp_commit_offset(timer);
  }
}

/** @brief Per-tick advance for every running timer unit. */
static void gptp_tick(uc_engine* uc)
{
  (void)uc;
  for (uint32_t t = 0U; t < (uint32_t)k_ra8_gptp_timer_count; ++t) {
    gptp_tick_timer(t);
  }
}

/** @brief Reset the GPTP model to its power-on state. */
static void gptp_reset(void)
{
  s_gptp = (gptp_state_t){};
}

/** @brief End-of-run GPTP section: report any unit that advanced. */
static void gptp_report(void)
{
  for (uint32_t t = 0U; t < (uint32_t)k_ra8_gptp_timer_count; ++t) {
    const gptp_timer_t* u = &s_gptp.timer[t];
    if (!u->enabled && (u->acc_sec == 0U) && (u->acc_fixed == 0U)) {
      continue; /* this unit never ran -- stay quiet. */
    }
    uint64_t sec  = 0U;
    uint32_t nsec = 0U;
    gptp_time_now(t, &sec, &nsec);
    (void)fprintf(stderr,
                  "  GPTP timer%u   : gPTP=%llu.%09u s (ESWCLK %llu MHz free-run)\n",
                  (unsigned)t,
                  (unsigned long long)sec,
                  (unsigned)nsec,
                  (unsigned long long)((uint64_t)k_gptp_eswclk_hz / (uint64_t)k_gptp_hz_per_mhz));
  }
}

/** @brief GPTP block descriptor. */
static const board_periph_block_t k_gptp_block = {
  .base   = (uint64_t)k_gptp_win_base,
  .span   = (uint64_t)k_gptp_win_span,
  .order  = (uint32_t)k_gptp_block_order,
  .read   = gptp_read,
  .write  = gptp_write,
  .tick   = gptp_tick,
  .reset  = gptp_reset,
  .report = gptp_report,
  .name   = "GPTP",
};

/** @brief Self-register the GPTP window before main runs (decentralized). */
[[gnu::constructor]] static void gptp_block_register(void)
{
  board_periph_register_block(&k_gptp_block);
}
