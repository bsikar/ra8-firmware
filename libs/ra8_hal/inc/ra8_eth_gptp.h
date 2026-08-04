/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_eth_gptp.h
 * @brief Ethernet Generic PTP Timer (GPTP) driver -- HUM Ch 35
 * @ingroup grp_hal_net
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Drives the RA8D2 GPTP block described by HUM Ch 35 "Ethernet Generic PTP
 * Timer (GPTP)" p 1925-1964. That block is a **timer**, and this driver
 * exposes exactly what the timer does:
 *
 *  - enable / disable each of the two timer units (`PTPTMEC` / `PTPTMDC`);
 *  - program the per-clk increment `PTPTIVCt` (nanoseconds per `clk` cycle,
 *    5.27 fixed point) from the ESWCLK frequency;
 *  - load the additive 78-bit offset `{PTPTOVCtU, PTPTOVCtM, PTPTOVCtL}`;
 *  - read the 78-bit GPTP time `{PTPGPTPTMtU, PTPGPTPTMtM, PTPGPTPTMtL}`
 *    (48-bit seconds + 30-bit nanoseconds);
 *  - read the 64-bit AVTP nanosecond view `{PTPAVTPTMtU, PTPAVTPTMtL}`;
 *  - read the read-only `PTPIPV` IP-version word, which is the only
 *    reset-nonzero register in the window and therefore the one honest
 *    "is this aperture really the GPTP block?" probe.
 *
 * @warning **There is no PTP message engine on this part.** HUM Ch 35
 * defines no Sync / Announce / Follow_Up generator, no domain register, no
 * clockIdentity, no port role and no BMCA -- so no such call exists here,
 * and none can. An IEEE 1588-2019 / IEEE 802.1AS implementation on RA8D2 is
 * software layered *above* this HAL: it disciplines this counter through
 * ::ra8_eth_gptp_set_offset and ::ra8_eth_gptp_set_increment, and takes its
 * receive timestamps from the RMAC capture path (`MTRC` / `MPFCt`,
 * HUM Ch 33) rather than from this block.
 *
 * The media-clock capture / recovery, cyclic-compare and pulse-output-timer
 * parts of the same register window need the MEDIA_IN / MEDIA_OUT /
 * CYCLIC_COMP pins, which the EK-RA8D2 board layer does not route, so this
 * driver does not address them.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_ether_regs.h"

/**
 * @enum ra8_eth_gptp_limit_t
 * @brief Field limits taken from the HUM register descriptions.
 *
 * @details
 * `PTPTOVCtL.TOVL` accepts only 0x0000_0000..0x3B9A_C9FF (HUM 35.3.2.4
 * p 1929), i.e. a nanosecond value strictly below one second, and
 * `PTPTIVCt.TIV` is a 5.27 fixed-point nanoseconds-per-clk value
 * (HUM 35.3.2.3 p 1928).
 *
 * @invariant ::k_ra8_gptp_nsec_max is exactly ::k_ra8_gptp_ns_per_sec - 1.
 *
 * @par Example:
 * @code
 * if (nsec > (uint32_t)k_ra8_gptp_nsec_max) { return k_ra8_err_invalid_arg; }
 * @endcode
 *
 * @see ra8_eth_gptp_set_offset
 */
typedef enum : uint32_t {
  k_ra8_gptp_ns_per_sec = 1000000000UL, /**< Nanoseconds in one second.      */
  k_ra8_gptp_nsec_max   = 999999999UL,  /**< Largest legal `PTPTOVCtL.TOVL`. */
} ra8_eth_gptp_limit_t;

/**
 * @enum ra8_eth_gptp_sec_limit_t
 * @brief Upper bound of the GPTP seconds field.
 *
 * @details
 * The GPTP time is 78 bits with the second part in `GPTP[t][77:30]`
 * (HUM 35.1 Table 35.1 p 1925), so seconds are 48 bits wide:
 * `{PTPTOVCtU.TOVU[15:0], PTPTOVCtM.TOVM[31:0]}`.
 *
 * @invariant Equals 2^48 - 1.
 *
 * @par Example:
 * @code
 * if (sec > (uint64_t)k_ra8_gptp_sec_max) { return k_ra8_err_invalid_arg; }
 * @endcode
 *
 * @see ra8_eth_gptp_set_offset
 */
typedef enum : uint64_t {
  k_ra8_gptp_sec_max = 0xFFFFFFFFFFFFULL, /**< Largest 48-bit seconds value. */
} ra8_eth_gptp_sec_limit_t;

/**
 * @struct ra8_eth_gptp_cfg_t
 * @brief Static configuration for ::ra8_eth_gptp_init.
 *
 * @details
 * The GPTP counter advances by `PTPTIVCt.TIV` on every `clk` edge, and `clk`
 * for the GPTP block is ESWCLK, the ESWM operating clock (HUM 9.10.23
 * "EtherSW Clock (ESWCLK)" p 398). Pass the live frequency -- callers get it
 * from ``ra8_cgc_eswclk_hz()`` -- and ::ra8_eth_gptp_init derives TIV from
 * it, so the counter is right on any clock tree rather than only on the one
 * the driver was written against.
 *
 * @invariant ``clk_hz`` must be high enough that TIV fits in 32 bits, i.e.
 *            strictly greater than 31.25 MHz.
 *
 * @par Example:
 * @code
 * uint32_t hz = 0U;
 * (void)ra8_cgc_eswclk_hz(&hz);
 * const ra8_eth_gptp_cfg_t cfg = {.clk_hz = hz};
 * @endcode
 *
 * @see ra8_eth_gptp_init
 */
typedef struct {
  uint32_t clk_hz; /**< ESWCLK frequency in Hz feeding the GPTP counter. */
} ra8_eth_gptp_cfg_t;

/**
 * @brief Power up the GPTP block and programme both timers' increment.
 *
 * @details
 * Releases the ESWM module-stop gate (the GPTP window is inside the
 * Layer-3 Ethernet Switch domain, MSTPC30), then for each of the two timer
 * units: stops it via `PTPTMDC`, writes the derived `PTPTIVCt` and clears
 * the 78-bit offset. Timers are left **stopped**; call
 * ::ra8_eth_gptp_timer_enable to start counting.
 *
 * @param[in] cfg Static configuration. Must not be ``nullptr``; ``clk_hz``
 *                must be greater than 31250000 (see ::ra8_eth_gptp_cfg_t).
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                 Block powered, both timers programmed.
 * @retval k_ra8_err_null_ptr       ``cfg == nullptr``.
 * @retval k_ra8_err_invalid_arg    ``cfg->clk_hz == 0``.
 * @retval k_ra8_err_out_of_range   ``cfg->clk_hz`` too low for a 32-bit TIV.
 * @retval k_ra8_err_hw_timeout     The ESWM module-stop release did not settle.
 *
 * @pre The ESWCLK is running and stable (HUM 11.2.8 note 6 p 447).
 * @pre Single-threaded init context, or IRQs masked.
 * @post On success both timer units hold the derived `PTPTIVCt`.
 * @post On success both timer units are stopped and read time zero.
 * @post On success every other entry point in this driver is unlocked.
 *
 * @note Not thread-safe; caller must provide synchronisation.
 *
 * @par Example:
 * @code
 * const ra8_eth_gptp_cfg_t cfg = {.clk_hz = 125000000U};
 * (void)ra8_eth_gptp_init(&cfg);
 * @endcode
 *
 * @see ra8_eth_gptp_timer_enable  Start a programmed timer
 * @see ra8_eth_gptp_deinit        Reverse this call
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_init(const ra8_eth_gptp_cfg_t* cfg);

/**
 * @brief Stop both timers, clear their configuration and gate the block off.
 *
 * @details
 * The full teardown: writes both `PTPTMDC.TD` bits, restores the reset value
 * of `PTPTIVCt` and of the 78-bit offset for both units, then re-asserts the
 * ESWM module stop. Use ::ra8_eth_gptp_enter_stop instead when the
 * configuration should survive the power transition.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Timers stopped and the gate re-asserted.
 * @retval k_ra8_err_hw_timeout    The module-stop assert did not settle.
 * @retval k_ra8_err_invalid_state The ESWM module-stop refcount was already 0
 *                                 (no matching ::ra8_eth_gptp_init).
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has run (otherwise the write faults).
 * @pre No consumer is mid-read of the timer.
 * @post Both timer units are stopped with `PTPTIVCt` and the offset at 0.
 * @post The ESWM module-stop bit is set once this driver is the last holder
 *       of the shared, reference-counted ESWM gate -- ``ra8_etha_init``
 *       takes the same gate, so with ETHA up the clock stays on.
 *
 * @note Not thread-safe; caller must provide synchronisation.
 *
 * @see ra8_eth_gptp_init  The matching bring-up call
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_deinit(void);

/**
 * @brief Read the read-only `PTPIPV` IP-version word.
 *
 * @details
 * `PTPIPV` (HUM 35.3.1.1 p 1927) is the only register in the GPTP window
 * with a nonzero reset value, so a nonzero read is positive evidence that
 * the aperture really is the GPTP block rather than a reserved region
 * echoing back writes.
 *
 * @param[out] out_version Receives `PTPIPV.IPV`. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok           Version word copied out.
 * @retval k_ra8_err_null_ptr ``out_version == nullptr``.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre ``out_version`` points at writable storage.
 * @post ``*out_version`` holds the register value on success.
 * @post No GPTP state was mutated (the register is read-only).
 *
 * @note Safe to call from any context; performs one MMIO read.
 *
 * @see ra8_eth_gptp_init  Must run first
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_ip_version(uint32_t* out_version);

/**
 * @brief Derive `PTPTIVCt.TIV` from a `clk` frequency.
 *
 * @details
 * `TIV` is the nanoseconds-per-`clk` increment in 5.27 fixed point:
 * `TIV[31:27]` is the whole-nanosecond part and `TIV[26:0]` the
 * sub-nanosecond part (HUM 35.3.2.3 p 1928), so
 * `TIV = round(1e9 * 2^27 / clk_hz)`. The HUM's worked examples
 * (0xA000_0000 / 0x5000_0000 / 0x2800_0000 / 0x2000_0000 for
 * 50 / 100 / 200 / 250 MHz) fall out of that expression exactly.
 *
 * This is a pure function -- it touches no hardware -- so an application
 * can sanity-check its clock tree before powering the block on.
 *
 * @param[in]  clk_hz  `clk` frequency in Hz. Must be nonzero and above
 *                     31250000, below which TIV would not fit in 32 bits.
 * @param[out] out_tiv Receives the computed increment. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok               Increment computed.
 * @retval k_ra8_err_null_ptr     ``out_tiv == nullptr``.
 * @retval k_ra8_err_invalid_arg  ``clk_hz == 0``.
 * @retval k_ra8_err_out_of_range Computed TIV exceeds 32 bits.
 *
 * @pre ``out_tiv`` points at writable storage.
 * @pre ``clk_hz`` describes the live ESWCLK, not a nominal figure.
 * @post ``*out_tiv`` is unmodified unless ``k_ra8_ok`` is returned.
 * @post No hardware register was read or written.
 *
 * @note Pure and re-entrant.
 *
 * @par Example:
 * @code
 * uint32_t tiv = 0U;
 * (void)ra8_eth_gptp_tiv_from_hz(125000000U, &tiv); // tiv == 0x40000000
 * @endcode
 *
 * @see ra8_eth_gptp_set_increment  Apply the value to a timer
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_tiv_from_hz(uint32_t clk_hz, uint32_t* out_tiv);

/**
 * @brief Start one timer unit (`PTPTMEC.TEq`).
 *
 * @details
 * Writes 1 to `PTPTMEC.TEq` (HUM 35.3.2.1 p 1927). The register is
 * write-1-to-set, so the sibling unit's enable bit is untouched. Counting
 * resumes from the next `clk` edge at the unit's current `PTPTIVCt`, on top of
 * whatever 78-bit offset ::ra8_eth_gptp_set_offset last committed.
 *
 * @param[in] timer Timer index, below ::k_ra8_gptp_timer_count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Timer enabled.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has programmed `PTPTIVCt` for this timer.
 * @pre The ESWCLK is running.
 * @post `PTPTMEC.TE[timer]` reads 1.
 * @post The unit's free-running counter advances by TIV per `clk`.
 *
 * @note Not thread-safe against ::ra8_eth_gptp_timer_disable.
 *
 * @see ra8_eth_gptp_timer_disable  Stop the unit again
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_timer_enable(ra8_gptp_timer_idx_t timer);

/**
 * @brief Stop one timer unit (`PTPTMDC.TDq`).
 *
 * @details
 * Per HUM 35.3.2.1 p 1927 a disabled unit's counters are fixed at 0, so a
 * stop is also a clear.
 *
 * @param[in] timer Timer index, below ::k_ra8_gptp_timer_count.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Timer disabled.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre No consumer is mid-read of this unit.
 * @post `PTPTMEC.TE[timer]` reads 0.
 * @post The unit's counters read 0.
 *
 * @note Not thread-safe against ::ra8_eth_gptp_timer_enable.
 *
 * @see ra8_eth_gptp_timer_enable  Start the unit again
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_timer_disable(ra8_gptp_timer_idx_t timer);

/**
 * @brief Report whether one timer unit is enabled.
 *
 * @details
 * Reads `PTPTMEC` once and extracts bit ``timer`` (HUM 35.3.2.1 p 1927).
 * `PTPTMDC` is write-only and reads as 0, so `PTPTMEC` is the single authority
 * on whether a unit is running: a unit stopped through `PTPTMDC` reads back 0
 * here.
 *
 * @param[in]  timer       Timer index, below ::k_ra8_gptp_timer_count.
 * @param[out] out_enabled Receives `PTPTMEC.TE[timer]`. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              State copied out.
 * @retval k_ra8_err_null_ptr    ``out_enabled == nullptr``.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre ``out_enabled`` points at writable storage.
 * @post ``*out_enabled`` holds the live enable bit on success.
 * @post No GPTP state was mutated.
 *
 * @note Performs one MMIO read; safe from any context.
 *
 * @see ra8_eth_gptp_timer_enable  Set the bit this reports
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_timer_is_enabled(ra8_gptp_timer_idx_t timer,
                                                      bool*                out_enabled);

/**
 * @brief Write one timer unit's increment register `PTPTIVCt`.
 *
 * @details
 * `PTPTIVCt` is writable at any time (HUM Table 35.4 p 1946-1947), which is
 * how a software servo applies a frequency correction: nudging TIV away from
 * ::ra8_eth_gptp_tiv_from_hz makes the counter run fast or slow.
 *
 * @param[in] timer Timer index, below ::k_ra8_gptp_timer_count.
 * @param[in] tiv   Nanoseconds per `clk` in 5.27 fixed point.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Increment written.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range, or ``tiv == 0``.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre ``tiv`` came from ::ra8_eth_gptp_tiv_from_hz or a servo around it.
 * @post `PTPTIVCt` holds ``tiv``.
 * @post The counter rate changes from the next `clk` edge.
 *
 * @note Not thread-safe against another writer of the same timer.
 *
 * @see ra8_eth_gptp_get_increment  Read the value back
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_set_increment(ra8_gptp_timer_idx_t timer, uint32_t tiv);

/**
 * @brief Read one timer unit's increment register `PTPTIVCt`.
 *
 * @details
 * Reads back whatever ::ra8_eth_gptp_set_increment last wrote, or the value
 * ::ra8_eth_gptp_init derived from ``clk_hz``, in the 5.27 fixed-point
 * encoding of HUM 35.3.2.3 p 1928. A software servo reads its current
 * correction through this call before computing the next one.
 *
 * @param[in]  timer   Timer index, below ::k_ra8_gptp_timer_count.
 * @param[out] out_tiv Receives `PTPTIVCt.TIV`. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Increment copied out.
 * @retval k_ra8_err_null_ptr    ``out_tiv == nullptr``.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre ``out_tiv`` points at writable storage.
 * @post ``*out_tiv`` holds the live register value on success.
 * @post No GPTP state was mutated.
 *
 * @note Performs one MMIO read; safe from any context.
 *
 * @see ra8_eth_gptp_set_increment  Write the value this reports
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_get_increment(ra8_gptp_timer_idx_t timer, uint32_t* out_tiv);

/**
 * @brief Load one timer unit's additive 78-bit offset.
 *
 * @details
 * Writes `PTPTOVCtU`, then `PTPTOVCtM`, then `PTPTOVCtL` -- the order is
 * load-bearing, because writing the L register is what commits the whole
 * offset to the timer (HUM 35.4.1.3.1 Figure 35.4 p 1944-1945). The reported
 * time is `offset + free-running counter` (HUM 35.5.1.1 p 1951), so this is
 * the coarse "step" half of a software clock servo.
 *
 * @param[in] timer Timer index, below ::k_ra8_gptp_timer_count.
 * @param[in] sec   Seconds part, at most ::k_ra8_gptp_sec_max.
 * @param[in] nsec  Nanoseconds part, at most ::k_ra8_gptp_nsec_max.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Offset committed.
 * @retval k_ra8_err_invalid_arg ``timer``, ``sec`` or ``nsec`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre The caller accepts a discontinuity in the reported time.
 * @post The three offset registers hold the requested value.
 * @post The next ::ra8_eth_gptp_get_time reflects the new offset.
 *
 * @note Not thread-safe; the three writes are not atomic against a reader.
 *
 * @par Example:
 * @code
 * (void)ra8_eth_gptp_set_offset(k_ra8_gptp_timer_0, 1000000000ULL, 0U);
 * @endcode
 *
 * @see ra8_eth_gptp_get_time  Observe the effect
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_eth_gptp_set_offset(ra8_gptp_timer_idx_t timer, uint64_t sec, uint32_t nsec);

/**
 * @brief Read one timer unit's 78-bit GPTP time.
 *
 * @details
 * Reads `PTPGPTPTMtL` first: that read latches `PTPGPTPTMtM` and
 * `PTPGPTPTMtU`, so the following two reads belong to the same instant
 * (HUM 35.4.1.3.4 Figure 35.7 p 1946). While the unit is disabled every
 * field reads 0 (HUM 35.3.2.1 p 1927), so a stopped timer is reported as
 * time zero rather than as an error.
 *
 * ``*out_nsec`` is masked to the register's 30 bits, which is wider than the
 * one-second range the counter should ever hold. A value at or above 1e9 is
 * therefore passed through rather than rejected: on a healthy block it cannot
 * occur, and on a faulted one the caller is better served seeing the raw
 * reading than a synthesised error.
 *
 * @param[in]  timer    Timer index, below ::k_ra8_gptp_timer_count.
 * @param[out] out_sec  Receives the 48-bit seconds part. Must not be ``nullptr``.
 * @param[out] out_nsec Receives the 30-bit nanoseconds part. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Time copied out.
 * @retval k_ra8_err_null_ptr    Either output pointer is ``nullptr``.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre Both output pointers address writable storage.
 * @post ``*out_sec`` and ``*out_nsec`` describe one latched instant.
 * @post No GPTP state was mutated beyond the M/U latch.
 *
 * @note Not re-entrant: a concurrent reader would steal the M/U latch.
 *
 * @par Example:
 * @code
 * uint64_t sec = 0U;
 * uint32_t nsec = 0U;
 * (void)ra8_eth_gptp_get_time(k_ra8_gptp_timer_0, &sec, &nsec);
 * @endcode
 *
 * @see ra8_eth_gptp_set_offset  Move the reported time
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_eth_gptp_get_time(ra8_gptp_timer_idx_t timer, uint64_t* out_sec, uint32_t* out_nsec);

/**
 * @brief Read one timer unit's 64-bit AVTP nanosecond counter.
 *
 * @details
 * Reads `PTPAVTPTMtL` first, which latches `PTPAVTPTMtU`
 * (HUM 35.4.1.3.3 Figure 35.6 p 1945-1946). The AVTP view is the same clock
 * as ::ra8_eth_gptp_get_time flattened to nanoseconds modulo 2^64
 * (HUM 35.5.1.2 p 1952) and is what an AVB talker stamps into presentation
 * times.
 *
 * @param[in]  timer  Timer index, below ::k_ra8_gptp_timer_count.
 * @param[out] out_ns Receives the 64-bit nanosecond count. Must not be ``nullptr``.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok              Counter copied out.
 * @retval k_ra8_err_null_ptr    ``out_ns == nullptr``.
 * @retval k_ra8_err_invalid_arg ``timer`` out of range.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre ``out_ns`` points at writable storage.
 * @post ``*out_ns`` describes one latched instant.
 * @post No GPTP state was mutated beyond the U latch.
 *
 * @note Not re-entrant: a concurrent reader would steal the U latch.
 *
 * @see ra8_eth_gptp_get_time  The 78-bit view of the same counter
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_get_avtp_ns(ra8_gptp_timer_idx_t timer, uint64_t* out_ns);

/**
 * @brief Stop both timers and assert the ESWM module-stop gate.
 *
 * @details
 * The low-power entry path. Unlike ::ra8_eth_gptp_deinit this leaves
 * `PTPTIVCt` and the 78-bit offset intact -- a module stop is a clock gate,
 * not a reset -- so ::ra8_eth_gptp_exit_stop followed by
 * ::ra8_eth_gptp_timer_enable resumes on the same configuration.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok                Timers stopped, gate asserted.
 * @retval k_ra8_err_hw_timeout    The module-stop assert did not settle.
 * @retval k_ra8_err_invalid_state The ESWM module-stop refcount was already 0
 *                                 (no matching ::ra8_eth_gptp_init).
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_init has released the module-stop gate.
 * @pre No consumer is mid-read of either timer.
 * @post Both timer units are stopped, with their configuration retained.
 * @post The ESWM module-stop bit is set once this driver is the last holder
 *       of the shared, reference-counted ESWM gate -- ``ra8_etha_init``
 *       takes the same gate, so with ETHA up the clock stays on.
 *
 * @note Not thread-safe; caller must provide synchronisation.
 *
 * @see ra8_eth_gptp_exit_stop  The matching resume call
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_enter_stop(void);

/**
 * @brief Release the ESWM module-stop gate again.
 *
 * @details
 * Leaves both timers stopped and their configuration as
 * ::ra8_eth_gptp_enter_stop left it; call ::ra8_eth_gptp_timer_enable to
 * resume counting.
 *
 * @return ra8_err_t Error code.
 * @retval k_ra8_ok             Gate released.
 * @retval k_ra8_err_hw_timeout The module-stop release did not settle.
 * @retval k_ra8_err_not_initialized ::ra8_eth_gptp_init has not run.
 *
 * @pre ::ra8_eth_gptp_enter_stop asserted the gate.
 * @pre The ESWCLK is running and stable.
 * @post The GPTP register window is addressable again (the ESWM gate is
 *       reference-counted, so it may already have been released by another
 *       holder such as ``ra8_etha_init``).
 * @post Both timer units remain stopped.
 *
 * @note Not thread-safe; caller must provide synchronisation.
 *
 * @see ra8_eth_gptp_enter_stop  The matching suspend call
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_eth_gptp_exit_stop(void);

#ifdef __cplusplus
}
#endif
