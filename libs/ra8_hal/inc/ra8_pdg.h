/**
 * @file ra8_pdg.h
 * @brief PWM Delay Generation Circuit (PDG) driver public API
 * @ingroup grp_hal_system
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * The PDG (HUM Ch 23 "PWM Delay Generation Circuit (PDG)",
 * p 1152-1163) is a per-edge fine-delay sub-block that sits in line
 * with the GPT320..GPT323 PWM outputs and shifts each rising or
 * falling edge by an integer fraction of the GPT core clock period
 * (1/128 in the 80..160 MHz range, 1/64 in the 155..300 MHz range).
 *
 * Hardware binding:
 *
 * - PDG channel 0 -> GPT320 -> GTIOC0A / GTIOC0B
 * - PDG channel 1 -> GPT321 -> GTIOC1A / GTIOC1B
 * - PDG channel 2 -> GPT322 -> GTIOC2A / GTIOC2B
 * - PDG channel 3 -> GPT323 -> GTIOC3A / GTIOC3B
 *
 * GPT324..GPT329 are not delayed -- they bypass the PDG entirely
 * (HUM Figure 23.1 p 1153). The driver therefore caps the channel
 * argument at 4.
 *
 * Initialization sequence implemented by ``ra8_pdg_init`` follows
 * Figure 23.2 (p 1160):
 *
 * 1. Clear DLLEN, set DLYRST = 1, all DLYBSn = 0, set FRANGE.
 * 2. Set DLLEN = 1 (start the DLL).
 * 3. Wait >= 20 us for the DLL to lock.
 * 4. Clear DLYRST (release the delay generation circuit).
 * 5. Wait >= 5 GTCLK cycles.
 * 6. Set DLYBSn = 1 for the channels in use (turn off bypass).
 *
 * Note: PDG is bound to GPT0/1/2/3 channels; you must also configure
 * the matching GPT timer through the GPT driver. Use this driver
 * solely for the per-edge fine delay overlay.
 *
 * Round-3 additions (full Ch 23 coverage):
 *
 * - Auto-tune calibration: pick FRANGE from a measured GPTCLK Hz.
 * - Multi-edge atomic batch updates: ``ra8_pdg_set_delay_batch``.
 * - Runtime FRANGE switch: ``ra8_pdg_set_frange`` re-runs the DLL
 * re-lock sequence with the new band, preserving channel state.
 * - Full GPT0/1/2/3 binding helpers: ``ra8_pdg_bind_gpt_channel``
 * cracks open GTWP for the matching GPT channel before letting
 * delay-register writes through (HUM 23.2.3 p 1156).
 * - Per-pin enable/disable helpers ``ra8_pdg_pin_bypass_set`` so a
 * single pin can stay bypassed while its sibling is delayed.
 * - Full status reporting: ``ra8_pdg_get_status_full`` returns a
 * structured snapshot of every PDG bit.
 * - Compare-match constraint check: ``ra8_pdg_check_constraints``
 * implements HUM Ch 23.4.2 Table 23.4 (p 1162).
 * - Register-write-interval helper ``ra8_pdg_required_write_ns``
 * implements HUM Ch 23.4.3 (p 1163).
 *
 * @see ra8_gpt.h GPT timer driver -- you must configure GPT320..323
 * output channels before the PDG delays take effect.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_pdg_regs.h"

/**
 * @enum ra8_pdg_edge_t
 * @brief Selects which delay register family is targeted.
 *
 * @details
 * Maps directly to the HUM split between GTDLYRn (rising) and
 * GTDLYFn (falling) registers. Used by ``ra8_pdg_set_delay`` /
 * ``ra8_pdg_get_delay``.
 */
typedef enum : uint8_t {
  k_ra8_pdg_edge_rising  = 0U, /**< GTDLYRnA / GTDLYRnB. */
  k_ra8_pdg_edge_falling = 1U, /**< GTDLYFnA / GTDLYFnB. */
} ra8_pdg_edge_t;

/**
 * @enum ra8_pdg_pin_t
 * @brief Picks the A or B pin within a PDG channel.
 *
 * @details
 * Each PDG channel feeds two GPT outputs: the A pin is GTIOCnA, the
 * B pin is GTIOCnB. The two delay codes are independent.
 */
typedef enum : uint8_t {
  k_ra8_pdg_pin_a = 0U, /**< GTIOCnA -- "A" output of GPT32n. */
  k_ra8_pdg_pin_b = 1U, /**< GTIOCnB -- "B" output of GPT32n. */
} ra8_pdg_pin_t;

/**
 * @enum ra8_pdg_status_t
 * @brief Bits surfaced by ``ra8_pdg_get_status`` (legacy compact form).
 *
 * @details
 * The PDG has no dedicated status register -- "status" here is a
 * snapshot of GTDLYCR (DLLEN / DLYRST / FRANGE) packed into a
 * caller-friendly mask. ``ra8_pdg_get_status`` reads GTDLYCR
 * directly per HUM Ch 23.2.1 p 1154.
 */
typedef enum : uint16_t {
  k_ra8_pdg_status_dll_locked = 0x0001U, /**< DLLEN = 1 (DLL running).    */
  k_ra8_pdg_status_in_reset   = 0x0002U, /**< DLYRST = 1 (held in reset). */
  k_ra8_pdg_status_frange_msk = 0x0300U, /**< Live FRANGE[1:0] bits.      */
} ra8_pdg_status_t;

/**
 * @enum ra8_pdg_count_dir_t
 * @brief Counter direction passed to ``ra8_pdg_check_constraints``.
 *
 * @details
 * HUM Ch 23.4.2 Table 23.4 (p 1162) restricts when a delay register
 * may be updated, depending on whether the host GPT counter is
 * counting up or down. The two values match the GPT GTST.TUCF bit
 * semantics so callers can pass GTST.TUCF directly.
 */
typedef enum : uint8_t {
  k_ra8_pdg_dir_up   = 0U, /**< Counter is up-counting (TUCF == 0).   */
  k_ra8_pdg_dir_down = 1U, /**< Counter is down-counting (TUCF == 1). */
} ra8_pdg_count_dir_t;

/**
 * @enum ra8_pdg_wave_mode_t
 * @brief PWM waveform shape -- selects which Table 23.4 row applies.
 *
 * @details
 * The constraint table in HUM Ch 23.4.2 (p 1162) lists two rows:
 * saw-wave mode and triangle-wave mode. Triangle mode is identified
 * here so that ``ra8_pdg_check_constraints`` selects the right row.
 */
typedef enum : uint8_t {
  k_ra8_pdg_wave_saw      = 0U, /**< Saw-wave PWM (count up or down). */
  k_ra8_pdg_wave_triangle = 1U, /**< Triangle-wave PWM.               */
} ra8_pdg_wave_mode_t;

/**
 * @struct ra8_pdg_config_t
 * @brief Driver configuration descriptor for ``ra8_pdg_init``.
 *
 * @details
 * cppcheck cannot see tests/ so it flags every field as unused;
 * each member is read in ``ra8_pdg_init`` in
 * ``libs/ra8_hal/src/ra8_pdg.c``.
 *
 * @invariant ``frange`` must be one of the ``k_ra8_pdg_frange_*``
 * enum values (the others are "Setting prohibited" per
 * HUM Ch 23.2.1 p 1154).
 *
 * @par Example:
 * @code
 * const ra8_pdg_config_t cfg = {
 *.frange = k_ra8_pdg_frange_80_160_mhz,
 *.channel_mask = 0x0F, // all 4 channels
 *.auto_tune = false,
 *.gptclk_hz = 0,
 * };
 * (void)ra8_pdg_init(&cfg);
 * @endcode
 */
typedef struct {
  ra8_pdg_frange_t frange;       /**< GPT core clock band (HUM 23.2.1).         */
  uint8_t          channel_mask; /**< Bit n = enable PDG channel n.             */
  uint8_t          auto_tune;    /**< 1 = ignore frange, derive from gptclk_hz. */
  uint32_t         gptclk_hz;    /**< Measured GTCLK frequency (Hz).            */
} ra8_pdg_config_t;

/**
 * @struct ra8_pdg_status_full_t
 * @brief Decoded snapshot of GTDLYCR / GTDLYCR2 returned by
 * ``ra8_pdg_get_status_full``.
 *
 * @details
 * Mirrors every documented bit in HUM Ch 23.2.1 (p 1154) and
 * HUM Ch 23.2.2 (p 1155). ``per_channel_bypass[n]`` and
 * ``per_channel_power[n]`` are decoded bool arrays so the caller
 * does not need to know the inverted DLYEN polarity.
 */
typedef struct {
  uint8_t          dll_enabled;                                     /**< GTDLYCR.DLLEN.         */
  uint8_t          in_reset;                                        /**< GTDLYCR.DLYRST.        */
  ra8_pdg_frange_t frange;                                          /**< GTDLYCR.FRANGE[1:0].   */
  uint8_t          per_channel_bypass_off[k_ra8_pdg_channel_count]; /**< DLYBSn.                */
  uint8_t          per_channel_powered[k_ra8_pdg_channel_count];    /**< !DLYENn.               */
  uint16_t         raw_gtdlycr;                                     /**< Raw GTDLYCR contents.  */
  uint16_t         raw_gtdlycr2;                                    /**< Raw GTDLYCR2 contents. */
} ra8_pdg_status_full_t;

/**
 * @struct ra8_pdg_delay_entry_t
 * @brief Single-edge descriptor for the batch update API.
 *
 * @details
 * A flat array of these is handed to ``ra8_pdg_set_delay_batch``
 * which writes them all under one critical section so the temporary
 * registers stay coherent until the next GPT overflow / underflow
 * / trough (HUM Ch 23.3.2 Figure 23.3 p 1161).
 */
typedef struct {
  uint8_t        channel; /**< 0..3.                              */
  ra8_pdg_pin_t  pin;     /**< k_ra8_pdg_pin_a / k_ra8_pdg_pin_b. */
  ra8_pdg_edge_t edge;    /**< Rising or falling.                 */
  uint8_t        code;    /**< DLY[6:0] code, 0..0x7F.            */
} ra8_pdg_delay_entry_t;

/**
 * @typedef ra8_pdg_event_fn_t
 * @brief PDG event callback (DLL lock event, etc.).
 *
 * @param[in] ctx Caller context forwarded from ``ra8_pdg_attach_handler``.
 *
 * @details
 * The PDG itself does not raise its own NVIC vector -- delay
 * transfers happen on GPT timer overflow / underflow / trough
 * (HUM Ch 23.3.2 p 1161). The callback hook is exposed so a host
 * test (or a future "DLL lost lock" watchdog wired through ELC)
 * has a single entry point to deliver async notifications.
 */
typedef void (*ra8_pdg_event_fn_t)(void* ctx);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Initialise the PDG block per HUM Figure 23.2 (p 1160).
 *
 * @details
 * Clears the PDG module-stop bit (MSTPD6 per HUM Ch 23.4.1 p 1162),
 * holds the circuit in reset, programs FRANGE (or auto-derives it
 * from ``cfg->gptclk_hz`` when ``cfg->auto_tune == 1``), enables
 * the DLL, waits for lock, releases reset, and turns off bypass
 * for the channels listed in ``cfg->channel_mask``.
 *
 * @param[in] cfg Non-NULL driver descriptor.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Init complete.
 * @retval k_ra8_err_null_ptr ``cfg`` was NULL.
 * @retval k_ra8_err_invalid_arg ``cfg->frange`` out of range or
 * ``cfg->channel_mask`` selects bits
 * outside [0..3] or auto-tune saw
 * gptclk_hz == 0.
 * @retval k_ra8_err_out_of_range Auto-tune saw gptclk_hz outside the
 * 80..300 MHz range allowed by HUM
 * Ch 23.2.1 p 1154.
 * @retval k_ra8_err_hw_init_failed MSTP failed to release the block.
 *
 * @pre IRQs masked or single-threaded init context.
 * @pre ``ra8_mstp_init`` has been called.
 * @post GTDLYCR.DLLEN = 1 and DLYRST = 0.
 * @post GTDLYCR2.DLYBSn = 1 for every n in ``cfg->channel_mask``.
 *
 * @note Thread safety: not thread-safe.
 * @see ra8_gpt.h
 * @see ra8_pdg_set_frange Switch FRANGE at runtime without deinit.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_init(const ra8_pdg_config_t* cfg);

/**
 * @brief Tear down the PDG block (reset + module-stop).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Block parked back in module-stop with all
 * channels disabled.
 *
 * @pre PDG was previously initialized (otherwise a no-op).
 * @post GTDLYCR.DLLEN = 0 and DLYRST = 1.
 * @post GTDLYCR2 = 0 (every channel bypassed and powered down).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_deinit(void);

/* =============================================================================
 * Per-edge delay programming
 * =============================================================================
 */

/**
 * @brief Set the per-edge fine delay for one PDG channel/pin/edge.
 *
 * @param[in] channel PDG channel (0..3, mapped to GPT320..GPT323).
 * @param[in] pin ``k_ra8_pdg_pin_a`` (GTIOCnA) or
 * ``k_ra8_pdg_pin_b`` (GTIOCnB).
 * @param[in] edge ``k_ra8_pdg_edge_rising`` or
 * ``k_ra8_pdg_edge_falling``.
 * @param[in] code DLY[6:0] code, 0..0x7F. 0 disables the delay
 * on this edge; the meaning of non-zero codes
 * depends on the FRANGE bit currently programmed
 * in GTDLYCR (HUM Ch 23.2.3..23.2.6 p 1156-1158).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Code written.
 * @retval k_ra8_err_invalid_arg Channel >= 4, pin/edge invalid, or
 * code > 0x7F.
 * @retval k_ra8_err_not_initialized PDG init has not run.
 *
 * @pre PDG is out of reset (GTDLYCR.DLYRST == 0).
 * @pre Caller is not currently inside the GPT compare-match window
 * where writes are disallowed (HUM Ch 23.4.2 p 1162, Table
 * 23.4) -- use ``ra8_pdg_check_constraints`` first.
 * @post The temporary register holds ``code``; it propagates to the
 * live delay on the next GPT overflow / underflow / trough
 * (HUM Ch 23.3.2 p 1161).
 *
 * @note Thread safety: not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_pdg_set_delay(uint8_t channel, ra8_pdg_pin_t pin, ra8_pdg_edge_t edge, uint8_t code);

/**
 * @brief Read back the last-programmed delay code.
 *
 * @param[in] channel PDG channel (0..3).
 * @param[in] pin ``k_ra8_pdg_pin_a`` / ``k_ra8_pdg_pin_b``.
 * @param[in] edge ``k_ra8_pdg_edge_rising`` / ``k_ra8_pdg_edge_falling``.
 * @param[out] out_code Receives the DLY[6:0] code last written.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``out_code`` is non-null.
 * @pre channel < 4.
 * @post ``*out_code`` is in 0..0x7F.
 *
 * @note Reads the live register, not the temporary buffer.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_pdg_get_delay(uint8_t channel, ra8_pdg_pin_t pin, ra8_pdg_edge_t edge, uint8_t* out_code);

/**
 * @brief Atomically program a batch of (channel, pin, edge, code)
 * delay updates.
 *
 * @details
 * Walks ``entries`` and applies each one with IRQs nominally masked
 * (host-test build is single-threaded). All writes land in the
 * PDG temporary registers in the same critical section, so the
 * next GPT overflow / underflow / trough applies them as one
 * coherent set. This is the multi-edge atomic batch entry-point
 * called for in HUM Ch 23.3.2 (p 1161).
 *
 * @param[in] entries Pointer to ``count`` entries.
 * @param[in] count Number of entries (must be > 0 and
 * <= ``k_ra8_pdg_slot_count``).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok All entries written.
 * @retval k_ra8_err_null_ptr ``entries`` is NULL.
 * @retval k_ra8_err_invalid_arg Count out of range or any entry
 * contains a bad channel / pin /
 * edge / code.
 * @retval k_ra8_err_not_initialized PDG init has not run.
 *
 * @pre ``entries`` non-null and points to ``count`` valid entries.
 * @pre PDG is out of reset.
 * @post Every entry's delay code is in the corresponding temporary
 * register; it propagates on next overflow.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_set_delay_batch(const ra8_pdg_delay_entry_t* entries,
                                                uint8_t                      count);

/**
 * @brief Convert a desired pin-to-pin delay (in nanoseconds) to a
 * DLY[6:0] code given a measured GPTCLK frequency.
 *
 * @details
 * Uses the divider table from HUM Ch 23.2.3..23.2.6 (p 1156-1158):
 * 1/128 step at ``k_ra8_pdg_frange_80_160_mhz``, 1/64 step at
 * ``k_ra8_pdg_frange_155_300_mhz``. The result is rounded to the
 * nearest representable code and clamped to ``[0, 0x7F]``.
 *
 * @param[in] delay_ns Desired delay in nanoseconds.
 * @param[in] gptclk_hz GPTCLK frequency in Hz (must be > 0).
 * @param[in] frange The FRANGE band currently programmed.
 * @param[out] out_code Receives the closest DLY[6:0] code.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Code computed.
 * @retval k_ra8_err_null_ptr ``out_code`` is NULL.
 * @retval k_ra8_err_invalid_arg ``gptclk_hz`` is zero or
 * ``frange`` out of range.
 *
 * @pre ``out_code`` non-null, gptclk_hz > 0.
 * @post ``*out_code`` <= 0x7F.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_delay_ns_to_code(uint32_t         delay_ns,
                                                 uint32_t         gptclk_hz,
                                                 ra8_pdg_frange_t frange,
                                                 uint8_t*         out_code);

/* =============================================================================
 * Per-channel and per-pin power transition
 * =============================================================================
 */

/**
 * @brief Power up one PDG channel (clear the GTDLYCR2.DLYENn bit).
 *
 * @param[in] channel 0..3.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PDG ``init`` has run.
 * @post Channel ``channel`` is power-on (HUM 23.2.2 p 1155 -- DLYEN
 * is inverted, 0 = enabled).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_exit_stop(uint8_t channel);

/**
 * @brief Power down one PDG channel (set GTDLYCR2.DLYENn).
 *
 * @param[in] channel 0..3.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre channel < 4.
 * @post Channel ``channel`` is power-off; its outputs revert to the
 * undelayed GPT signal.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_enter_stop(uint8_t channel);

/**
 * @brief Set or clear the bypass bit for a channel.
 *
 * @details
 * HUM Ch 23.2.2 (p 1155) describes the DLYBSn bit: 1 = delay
 * applied, 0 = bypass. ``ra8_pdg_init`` already sets the bit for
 * every channel in ``cfg->channel_mask``; this helper exists so a
 * caller can flip the bypass on the fly without a full re-init.
 *
 * @param[in] channel 0..3.
 * @param[in] bypass Non-zero sets bypass-off (delay applied);
 * zero sets bypass-on (delay skipped).
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PDG ``init`` has run.
 * @post DLYBSn matches ``bypass``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_channel_bypass_set(uint8_t channel, uint8_t bypass);

/**
 * @brief Force a single A or B pin's delay to zero (per-pin disable)
 * without touching the channel-level bypass / power bits.
 *
 * @details
 * The HUM lets DLYBSn / DLYENn be flipped per channel; per-pin
 * disable is achieved by writing 0x00 into the pin's two delay
 * cells (rising + falling). ``ra8_pdg_pin_disable`` does that, and
 * ``ra8_pdg_pin_enable`` restores caller-supplied codes.
 *
 * @param[in] channel 0..3.
 * @param[in] pin ``k_ra8_pdg_pin_a`` or ``k_ra8_pdg_pin_b``.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PDG ``init`` has run.
 * @post Both rising and falling delay cells of the chosen pin = 0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_pin_disable(uint8_t channel, ra8_pdg_pin_t pin);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Read GTDLYCR packed into a status mask.
 *
 * @param[out] out Receives a bitmask of ``ra8_pdg_status_t`` bits.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre ``out`` is non-null.
 * @post ``*out`` reflects the live GTDLYCR (DLLEN, DLYRST, FRANGE).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_get_status(uint16_t* out);

/**
 * @brief Read a fully-decoded snapshot of GTDLYCR + GTDLYCR2.
 *
 * @param[out] out Non-null structure to fill in.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Snapshot stored.
 * @retval k_ra8_err_null_ptr ``out`` is NULL.
 *
 * @pre ``out`` non-null.
 * @post Every field in ``*out`` reflects the live registers at call
 * time (DLLEN, DLYRST, FRANGE, DLYBSn, DLYENn).
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_get_status_full(ra8_pdg_status_full_t* out);

/**
 * @brief Clear the soft-reset bit (DLYRST -> 0) and re-enable the DLL.
 *
 * @details
 * Used to re-arm the block after a controlled park via
 * ``ra8_pdg_enter_stop``. The "status mask" is in fact the GTDLYCR
 * register so this call only writes back the bit that the caller
 * provided -- DLYRST is forced low.
 *
 * @param[in] mask Caller-supplied bits to clear in GTDLYCR. Only
 * ``k_ra8_pdg_status_in_reset`` is meaningful today.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre PDG ``init`` has run.
 * @post GTDLYCR.DLYRST = 0 if ``mask`` includes
 * ``k_ra8_pdg_status_in_reset``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_clear_status(uint16_t mask);

/**
 * @brief Attach an asynchronous PDG event callback.
 *
 * @details
 * The PDG itself does not raise an NVIC vector -- this hook is
 * available for higher-level code that wants to forward GPT
 * compare-match events (e.g. "delay updated") through a single
 * registration point.
 *
 * @param[in] fn Callback invoked from ``ra8_pdg_dispatch``.
 * @param[in] ctx Forwarded to ``fn``.
 *
 * @return ``k_ra8_ok`` -- the registration cannot fail.
 *
 * @pre None.
 * @post Subsequent ``ra8_pdg_dispatch`` calls invoke ``fn``.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_attach_handler(ra8_pdg_event_fn_t fn, void* ctx);

/**
 * @brief Fire the registered handler. Test / ELC entry point.
 *
 * @since 0.1.0
 *
 * @details See implementation.
 * @pre Module state is consistent.
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 * @post Caller-visible state matches the documented contract.
 * @note Not thread-safe unless documented otherwise.
 */
void ra8_pdg_dispatch(void);

/* =============================================================================
 * Runtime FRANGE switch + auto-tune calibration
 * =============================================================================
 */

/**
 * @brief Auto-pick a FRANGE value from a measured GPTCLK frequency.
 *
 * @details
 * Implements the band-selection table of HUM Ch 23.2.1 (p 1154):
 *
 * - 80..160 MHz -> ``k_ra8_pdg_frange_80_160_mhz``
 * - 155..300 MHz -> ``k_ra8_pdg_frange_155_300_mhz``
 *
 * The two ranges overlap between 155 and 160 MHz; this helper picks
 * the lower band when the input falls in the overlap because that
 * gives the finer 1/128 step.
 *
 * @param[in] gptclk_hz GPTCLK in Hz (>0). 80..300 MHz inclusive.
 * @param[out] out Receives the chosen ``ra8_pdg_frange_t``.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Band selected.
 * @retval k_ra8_err_null_ptr ``out`` is NULL.
 * @retval k_ra8_err_invalid_arg ``gptclk_hz`` is 0.
 * @retval k_ra8_err_out_of_range ``gptclk_hz`` outside [80 MHz, 300 MHz].
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_pick_frange(uint32_t gptclk_hz, ra8_pdg_frange_t* out);

/**
 * @brief Switch FRANGE at runtime, re-running the DLL re-lock
 * sequence with the new band.
 *
 * @details
 * HUM Ch 23.2.1 (p 1154): "Set the FRANGE[1:0] bits only when the
 * DLLEN bit is 0." This routine therefore performs:
 *
 * 1. Save current DLYBSn / DLYENn from GTDLYCR2.
 * 2. Drop DLLEN to 0, raise DLYRST to 1.
 * 3. Write the new FRANGE bits.
 * 4. Re-enable DLLEN, wait 20 us for lock.
 * 5. Lower DLYRST.
 * 6. Restore the saved GTDLYCR2 bits.
 *
 * @param[in] new_frange The new FRANGE encoding.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Switch complete.
 * @retval k_ra8_err_invalid_arg ``new_frange`` not a legal band.
 * @retval k_ra8_err_not_initialized PDG init has not run.
 *
 * @pre PDG ``init`` has run.
 * @post GTDLYCR.FRANGE == ``new_frange``, DLLEN = 1, DLYRST = 0.
 * @post GTDLYCR2 contents preserved across the call.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_set_frange(ra8_pdg_frange_t new_frange);

/* =============================================================================
 * GPT0/1/2/3 binding helpers
 * =============================================================================
 */

/**
 * @brief Bind PDG channel ``n`` to its host GPT32n channel.
 *
 * @details
 * HUM Ch 23.2.3 (p 1156) requires that GTWP write-protect is
 * cleared on the matching GPT channel before the PDG delay
 * registers can be written. This helper performs that GTWP unlock
 * (and stores the prior value so the caller can re-arm protection
 * later via ``ra8_pdg_unbind_gpt_channel``).
 *
 * @param[in] channel 0..3 (GPT320..GPT323).
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok GTWP cleared, binding active.
 * @retval k_ra8_err_invalid_arg channel out of range.
 *
 * @pre PDG ``init`` has run.
 * @post GPT32n.GTWP write-protect bits cleared.
 * @post Subsequent ra8_pdg_set_delay writes are accepted.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_bind_gpt_channel(uint8_t channel);

/**
 * @brief Reverse ``ra8_pdg_bind_gpt_channel``, restoring write-protect.
 *
 * @param[in] channel 0..3.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre channel < 4.
 * @post GPT32n.GTWP write-protect re-armed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_unbind_gpt_channel(uint8_t channel);

/* =============================================================================
 * Constraint validation (HUM Ch 23.4.2 / 23.4.3)
 * =============================================================================
 */

/**
 * @brief Check Table 23.4 constraints before writing a delay register.
 *
 * @details
 * HUM Ch 23.4.2 Table 23.4 (p 1162) forbids changing a delay
 * register while the host GPT is in a "no write" window:
 *
 * | mode | direction | compare-match value forbidden |
 * |-----------|-----------|-------------------------------|
 * | Saw | Up | >= GTPR - 2 |
 * | Saw | Down | <= 2 |
 * | Triangle | Down | <= 2 |
 *
 * @param[in] mode Saw vs triangle.
 * @param[in] dir Counter direction.
 * @param[in] compare_match Current GTCCRx (= compare match value).
 * @param[in] gtpr GPT period register value.
 *
 * @return ``ra8_err_t`` error code.
 * @retval k_ra8_ok Safe to write the delay register.
 * @retval k_ra8_err_invalid_state Inside the forbidden window.
 *
 * @pre None.
 * @post No side effects.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_check_constraints(ra8_pdg_wave_mode_t mode,
                                                  ra8_pdg_count_dir_t dir,
                                                  uint32_t            compare_match,
                                                  uint32_t            gtpr);

/**
 * @brief Compute the minimum register-write interval (HUM 23.4.3 p 1163).
 *
 * @details
 * Write_Interval [ns] = Period_of_PCLKA [ns] x 6
 * + Period_of_GPTCLK [ns] x 4
 *
 * Used by callers that issue back-to-back writes to the same delay
 * register to size their software pacing.
 *
 * @param[in] pclka_hz PCLKA frequency in Hz (>0).
 * @param[in] gptclk_hz GPTCLK frequency in Hz (>0).
 * @param[out] out_ns Receives the required interval, in ns.
 *
 * @return ``ra8_err_t`` error code.
 *
 * @pre Both clocks > 0; ``out_ns`` non-null.
 * @post ``*out_ns`` >= 0.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_pdg_required_write_ns(uint32_t pclka_hz, uint32_t gptclk_hz, uint32_t* out_ns);

/* =============================================================================
 * Sweep 17: capture-style buffered delay-code apply + completion callback
 * =============================================================================
 */

/**
 * @brief Stage a buffer of `ra8_pdg_delay_entry_t` records and arm them.
 *
 * @details
 * The PDG itself does not "capture" data -- this entry-point exists
 * so the camera/data-capture orchestrator can stream a buffer of
 * fine-delay updates through the same `*_capture_start(buf, len)`
 * shape that the VIN / CEU drivers expose. Each
 * `ra8_pdg_delay_entry_t` in `buf` is forwarded to
 * `ra8_pdg_set_delay_batch` after a NULL / range check, and the
 * registered completion callback (see `ra8_pdg_attach_handler`) fires
 * via `ra8_pdg_dispatch` once all entries land.
 *
 * @param[in] buf Pointer to an array of `ra8_pdg_delay_entry_t` records.
 * @param[in] len Element count (must be > 0 and <= `k_ra8_pdg_slot_count`).
 *
 * @return ra8_err_t
 * @retval k_ra8_ok                   Entries staged + dispatch fired.
 * @retval k_ra8_err_null_ptr         buf was NULL.
 * @retval k_ra8_err_invalid_arg      len out of range or bad entry.
 * @retval k_ra8_err_not_initialized  PDG init has not run.
 *
 * @pre PDG `init` has run.
 * @pre `buf` is non-NULL and points to `len` valid entries.
 * @post All entries are written to their PDG temporary registers.
 * @post The registered completion callback fires once.
 *
 * @note Not thread-safe.
 * @see ra8_pdg_set_delay_batch
 * @see ra8_pdg_capture_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_capture_start(const ra8_pdg_delay_entry_t* buf, uint8_t len);

/**
 * @brief Cancel any pending buffered apply and clear the completion latch.
 *
 * @details
 * Mirrors the VIN / CEU `*_capture_stop` shape. Implementation simply
 * clears the internal "in flight" flag so a subsequent
 * `ra8_pdg_dispatch` does not double-fire the callback.
 *
 * @return ra8_err_t
 * @retval k_ra8_ok Always.
 *
 * @pre PDG `init` has run.
 * @post Pending in-flight latch is cleared.
 *
 * @note Not thread-safe.
 * @see ra8_pdg_capture_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_pdg_capture_stop(void);

#ifdef __cplusplus
}
#endif
