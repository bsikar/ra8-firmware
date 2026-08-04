/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_gpt_capture.h
 * @brief GPT input-capture + external event / pulse-count extension
 * @ingroup grp_hal_timers
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Input-capture (GTICASR / GTICBSR) and external event-counting
 * (GTUPSR / GTDNSR) slice of the RA8D2 GPT driver, split out of
 * ``ra8_gpt.h`` to keep every header under the file-size budget.
 *
 * This header is a one-directional extension of ``ra8_gpt.h``: it includes
 * the base header (for ``ra8_gpt_ccr_sel_t``, ``ra8_err_t``, and the
 * status-flag enum); consumers that need the capture / event-count API
 * include this header directly. ``ra8_gpt.h`` does NOT include this header
 * back, so there is no include cycle.
 *
 * API surface:
 *
 * - ``ra8_gpt_capture_configure`` -- arm GTCCRA / GTCCRB for input capture
 * - ``ra8_gpt_capture_read`` -- read the latched GTCNT value
 * - ``ra8_gpt_event_count_configure`` -- external up / down edge counting
 */

#pragma once

#include "ra8_gpt.h"

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/**
 * @enum ra8_gpt_capture_source_t
 * @brief Trigger-source bitmask for input capture (GTICASR / GTICBSR).
 *
 * @details
 * GTICASR and GTICBSR share an identical bit layout (the ``AS`` prefix
 * latches GTCNT into GTCCRA, the ``BS`` prefix into GTCCRB). When at
 * least one bit of the selected register is set, the matching GTCCR
 * register stops acting as an output-compare register and instead
 * latches the live GTCNT value on every selected edge -- the primitive
 * used to measure an external signal's period, frequency, or
 * pulse-width. The captured 32-bit value is read back with
 * ``ra8_gpt_capture_read``; the TCFA / TCFB flag in GTST
 * (``k_ra8_gpt_status_ccra`` / ``k_ra8_gpt_status_ccrb``) is raised on
 * each capture. The GTIOCnA / GTIOCnB pin output is automatically
 * suppressed while its capture register is active, regardless of the
 * GTIOR output-enable bit.
 *
 * The ``ioca`` / ``iocb`` edge values set both the
 * "during the other pin low" and "during the other pin high" bits, so
 * the edge is captured unconditionally (the usual period-measure case).
 * Values may be OR-ed together to capture from several sources.
 *
 * @invariant Only bits inside ``k_ra8_gpt_cap_src_valid_mask`` are legal.
 * @see ra8_gpt_capture_configure()
 * @see ra8_gpt_capture_read()
 *
 * HUM Ch 22.2.10 "GTICASR : General PWM Timer Input Capture Source
 * Select Register A" p 906-908; HUM Ch 22.2.11 "GTICBSR : General PWM
 * Timer Input Capture Source Select Register B" p 909-912.
 */
typedef enum : uint32_t {
  k_ra8_gpt_cap_src_none            = 0x00000000UL, /**< No capture source (disable). */
  k_ra8_gpt_cap_src_gtetrga_rising  = 0x00000001UL, /**< bit 0  GTETRGA rising.       */
  k_ra8_gpt_cap_src_gtetrga_falling = 0x00000002UL, /**< bit 1  GTETRGA falling.      */
  k_ra8_gpt_cap_src_gtetrgb_rising  = 0x00000004UL, /**< bit 2  GTETRGB rising.       */
  k_ra8_gpt_cap_src_gtetrgb_falling = 0x00000008UL, /**< bit 3  GTETRGB falling.      */
  k_ra8_gpt_cap_src_gtetrgc_rising  = 0x00000010UL, /**< bit 4  GTETRGC rising.       */
  k_ra8_gpt_cap_src_gtetrgc_falling = 0x00000020UL, /**< bit 5  GTETRGC falling.      */
  k_ra8_gpt_cap_src_gtetrgd_rising  = 0x00000040UL, /**< bit 6  GTETRGD rising.       */
  k_ra8_gpt_cap_src_gtetrgd_falling = 0x00000080UL, /**< bit 7  GTETRGD falling.      */
  k_ra8_gpt_cap_src_ioca_rising     = 0x00000300UL, /**< bits 8,9   GTIOCnA rising.   */
  k_ra8_gpt_cap_src_ioca_falling    = 0x00000C00UL, /**< bits 10,11 GTIOCnA falling.  */
  k_ra8_gpt_cap_src_iocb_rising     = 0x00003000UL, /**< bits 12,13 GTIOCnB rising.   */
  k_ra8_gpt_cap_src_iocb_falling    = 0x0000C000UL, /**< bits 14,15 GTIOCnB falling.  */
  k_ra8_gpt_cap_src_elc_a           = 0x00010000UL, /**< bit 16 ELC_GPTA event.       */
  k_ra8_gpt_cap_src_elc_b           = 0x00020000UL, /**< bit 17 ELC_GPTB event.       */
  k_ra8_gpt_cap_src_elc_c           = 0x00040000UL, /**< bit 18 ELC_GPTC event.       */
  k_ra8_gpt_cap_src_elc_d           = 0x00080000UL, /**< bit 19 ELC_GPTD event.       */
  k_ra8_gpt_cap_src_elc_e           = 0x00100000UL, /**< bit 20 ELC_GPTE event.       */
  k_ra8_gpt_cap_src_elc_f           = 0x00200000UL, /**< bit 21 ELC_GPTF event.       */
  k_ra8_gpt_cap_src_elc_g           = 0x00400000UL, /**< bit 22 ELC_GPTG event.       */
  k_ra8_gpt_cap_src_elc_h           = 0x00800000UL, /**< bit 23 ELC_GPTH event.       */
  k_ra8_gpt_cap_src_other_channel   = 0x01000000UL, /**< bit 24 ASOC/BSOC (GPT4..9).  */
  k_ra8_gpt_cap_src_valid_mask      = 0x01FFFFFFUL, /**< bits 0..24: all legal bits.  */
} ra8_gpt_capture_source_t;

/**
 * @enum ra8_gpt_count_source_t
 * @brief Trigger-source bitmask for external event counting (GTUPSR / GTDNSR).
 *
 * @details
 * GTUPSR (count-up) and GTDNSR (count-down) share an identical bit
 * layout (the ``US`` / ``DS`` prefix). When at least one bit of either
 * register is set the channel enters event-count operation: GTCNT no
 * longer counts the internal PCLKD-derived clock (GTCR.TPCS is ignored)
 * and the GTCR.MD waveform mode is suppressed; instead GTCNT increments
 * on each selected up source and decrements on each selected down
 * source. Routing one edge to GTUPSR and the quadrature-paired edge to
 * GTDNSR turns the channel into an up/down encoder counter; routing a
 * single edge to GTUPSR only gives a plain external-pulse counter.
 *
 * The ``ioca`` / ``iocb`` edge values set both the "during the other
 * pin low" and "during the other pin high" bits so the edge counts
 * unconditionally. Values may be OR-ed together.
 *
 * @invariant Only bits inside ``k_ra8_gpt_cnt_src_valid_mask`` are legal.
 * @see ra8_gpt_event_count_configure()
 *
 * HUM Ch 22.2.8 "GTUPSR : General PWM Timer Up Count Source Select
 * Register" p 898-901; HUM Ch 22.2.9 "GTDNSR : General PWM Timer Down
 * Count Source Select Register" p 902-905.
 */
typedef enum : uint32_t {
  k_ra8_gpt_cnt_src_none            = 0x00000000UL, /**< No count source (disable).  */
  k_ra8_gpt_cnt_src_gtetrga_rising  = 0x00000001UL, /**< bit 0  GTETRGA rising.      */
  k_ra8_gpt_cnt_src_gtetrga_falling = 0x00000002UL, /**< bit 1  GTETRGA falling.     */
  k_ra8_gpt_cnt_src_gtetrgb_rising  = 0x00000004UL, /**< bit 2  GTETRGB rising.      */
  k_ra8_gpt_cnt_src_gtetrgb_falling = 0x00000008UL, /**< bit 3  GTETRGB falling.     */
  k_ra8_gpt_cnt_src_gtetrgc_rising  = 0x00000010UL, /**< bit 4  GTETRGC rising.      */
  k_ra8_gpt_cnt_src_gtetrgc_falling = 0x00000020UL, /**< bit 5  GTETRGC falling.     */
  k_ra8_gpt_cnt_src_gtetrgd_rising  = 0x00000040UL, /**< bit 6  GTETRGD rising.      */
  k_ra8_gpt_cnt_src_gtetrgd_falling = 0x00000080UL, /**< bit 7  GTETRGD falling.     */
  k_ra8_gpt_cnt_src_ioca_rising     = 0x00000300UL, /**< bits 8,9   GTIOCnA rising.  */
  k_ra8_gpt_cnt_src_ioca_falling    = 0x00000C00UL, /**< bits 10,11 GTIOCnA falling. */
  k_ra8_gpt_cnt_src_iocb_rising     = 0x00003000UL, /**< bits 12,13 GTIOCnB rising.  */
  k_ra8_gpt_cnt_src_iocb_falling    = 0x0000C000UL, /**< bits 14,15 GTIOCnB falling. */
  k_ra8_gpt_cnt_src_elc_a           = 0x00010000UL, /**< bit 16 ELC_GPTA event.      */
  k_ra8_gpt_cnt_src_elc_b           = 0x00020000UL, /**< bit 17 ELC_GPTB event.      */
  k_ra8_gpt_cnt_src_elc_c           = 0x00040000UL, /**< bit 18 ELC_GPTC event.      */
  k_ra8_gpt_cnt_src_elc_d           = 0x00080000UL, /**< bit 19 ELC_GPTD event.      */
  k_ra8_gpt_cnt_src_elc_e           = 0x00100000UL, /**< bit 20 ELC_GPTE event.      */
  k_ra8_gpt_cnt_src_elc_f           = 0x00200000UL, /**< bit 21 ELC_GPTF event.      */
  k_ra8_gpt_cnt_src_elc_g           = 0x00400000UL, /**< bit 22 ELC_GPTG event.      */
  k_ra8_gpt_cnt_src_elc_h           = 0x00800000UL, /**< bit 23 ELC_GPTH event.      */
  k_ra8_gpt_cnt_src_valid_mask      = 0x00FFFFFFUL, /**< bits 0..23: all legal bits. */
} ra8_gpt_count_source_t;

/**
 * @brief Arm a GPT channel for input capture on GTCCRA or GTCCRB.
 *
 * @details
 * Writes ``source_mask`` into GTICASR (when ``which`` is
 * ``k_ra8_gpt_ccr_a``) or GTICBSR (when ``which`` is ``k_ra8_gpt_ccr_b``)
 * inside a GTWP write-protect window. A non-zero mask switches the
 * selected GTCCR register from output-compare to input-capture: each
 * selected edge latches GTCNT into the register and raises the matching
 * GTST flag (read via ``ra8_gpt_get_status``). Passing
 * ``k_ra8_gpt_cap_src_none`` returns the register to output-compare use.
 *
 * The counter itself is unaffected -- start it on the internal clock
 * with ``ra8_gpt_init`` (or ``ra8_gpt_start_free_run``) so GTCNT advances
 * between captures; the difference between two captures is the measured
 * period in counter ticks.
 *
 * @param[in] channel     GPT channel 0..13.
 * @param[in] which       Target capture register: A (GTCCRA) or B (GTCCRB).
 * @param[in] source_mask OR of ``k_ra8_gpt_cap_src_*`` bits; must lie
 *                         within ``k_ra8_gpt_cap_src_valid_mask``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Capture source programmed.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range.
 * @retval k_ra8_err_invalid_arg ``which`` out of range or ``source_mask``
 *                              sets a reserved bit.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init`` /
 *      ``ra8_gpt_start_free_run``.
 * @pre IRQs masked or single-threaded init context.
 * @post GTICASR (A) or GTICBSR (B) equals ``source_mask``.
 * @post GTWP is re-locked.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_gpt_capture_read()  Read the latched GTCNT value.
 * @see ra8_gpt_get_status()    Poll the TCFA / TCFB capture flag.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.10 "GTICASR : General PWM Timer Input Capture Source Select
 * Register A" p 906-908. Ch 22.2.11 "GTICBSR : General PWM Timer Input
 * Capture Source Select Register B" p 909-912. Ch 22.2.1 "GTWP :
 * General PWM Timer Write Protection Register" p 883-884.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_capture_configure(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t source_mask);

/**
 * @brief Read the value latched by the last input capture.
 *
 * @details
 * Reads GTCCRA (``which`` == ``k_ra8_gpt_ccr_a``) or GTCCRB (``which`` ==
 * ``k_ra8_gpt_ccr_b``), which hold the GTCNT value latched on the most
 * recent capture edge while the matching source register is armed. The
 * caller typically polls ``ra8_gpt_get_status`` for TCFA / TCFB, reads
 * the value here, then clears the flag with ``ra8_gpt_clear_status``.
 *
 * @param[in]  channel   GPT channel 0..13.
 * @param[in]  which     Source capture register: A (GTCCRA) or B (GTCCRB).
 * @param[out] out_value Receives the latched 32-bit counter value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Value read into ``out_value``.
 * @retval k_ra8_err_null_ptr    ``out_value`` NULL or ``channel`` out of range.
 * @retval k_ra8_err_invalid_arg ``which`` out of range.
 *
 * @pre ``ra8_gpt_capture_configure`` armed the register being read.
 * @pre ``out_value`` points to writable storage.
 * @post ``*out_value`` holds the latched GTCCR value on success.
 * @post No register is modified (read-only path).
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_gpt_capture_configure()
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.20 "GTCCRk : General PWM Timer Compare Capture Register k
 * (k = A to F)" p 938.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_capture_read(uint8_t channel, ra8_gpt_ccr_sel_t which, uint32_t* out_value);

/**
 * @brief Configure external event (edge) counting on GTCNT.
 *
 * @details
 * Writes ``up_source`` into GTUPSR and ``down_source`` into GTDNSR
 * inside a GTWP write-protect window. When either register is non-zero
 * the channel leaves internal-clock counting and instead increments
 * GTCNT on each selected up edge and decrements it on each selected
 * down edge -- the primitive for external-pulse counting (up source
 * only) and quadrature-encoder decoding (paired up / down sources).
 * Passing ``k_ra8_gpt_cnt_src_none`` for both arguments returns the
 * channel to internal-clock counting.
 *
 * Start the counter separately (``ra8_gpt_init`` with ``auto_start`` or
 * ``ra8_gpt_start_free_run``); GTCR.CST still gates whether edges are
 * counted. GTCR.TPCS and the GTCR.MD waveform mode have no effect while
 * an event-count source is active.
 *
 * @param[in] channel     GPT channel 0..13.
 * @param[in] up_source   OR of ``k_ra8_gpt_cnt_src_*`` up-count bits;
 *                        must lie within ``k_ra8_gpt_cnt_src_valid_mask``.
 * @param[in] down_source OR of ``k_ra8_gpt_cnt_src_*`` down-count bits;
 *                        must lie within ``k_ra8_gpt_cnt_src_valid_mask``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok              Up / down sources programmed.
 * @retval k_ra8_err_null_ptr    ``channel`` out of range.
 * @retval k_ra8_err_invalid_arg ``up_source`` or ``down_source`` sets a
 *                              reserved bit.
 *
 * @pre Channel previously initialized via ``ra8_gpt_init``.
 * @pre IRQs masked or single-threaded init context.
 * @post GTUPSR == ``up_source`` and GTDNSR == ``down_source``.
 * @post GTWP is re-locked.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_gpt_read()  Read the accumulated GTCNT edge count.
 * @since 0.1.0
 *
 * @par HUM:
 * Ch 22.2.8 "GTUPSR : General PWM Timer Up Count Source Select
 * Register" p 898-901. Ch 22.2.9 "GTDNSR : General PWM Timer Down Count
 * Source Select Register" p 902-905. Ch 22.2.1 "GTWP : General PWM
 * Timer Write Protection Register" p 883-884.
 */
[[nodiscard]] ra8_err_t
ra8_gpt_event_count_configure(uint8_t channel, uint32_t up_source, uint32_t down_source);

#ifdef __cplusplus
}
#endif
