/**
 * @file ra8_mipi_dsi_api.h
 * @brief MIPI DSI-2 host driver -- public function surface (full HUM Ch 65)
 * @ingroup grp_hal_display
 *
 * @par Tag
 * [Ring 3 / HAL] {World: S}
 *
 * @details
 * Function-prototype split of the MIPI DSI host public API (HUM Ch 65,
 * p 3839-3934). Carries every lifecycle / HS-clock / sequence-channel /
 * ULPS / video-mode / status-IRQ / dispatch prototype plus the Sweep-6
 * convenience surface and its compact video-timing struct. Pulled out of
 * `ra8_mipi_dsi.h` so the umbrella stays a thin include shim; consumers
 * keep including `ra8_mipi_dsi.h` unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_mipi_dsi_regs.h"
#include "ra8_mipi_dsi_types.h"

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up the MIPI DSI host link layer.
 *
 * @details
 * 1. Releases module-stop bit `MSTPCRC.MSTPC10` (HUM 11.2.8 p 446).
 * 2. Asserts then de-asserts `RSTCR.SWRST` to drop the block back to
 *    a known reset state.
 * 3. Programmes lane count + lane enables in `TXSETR`.
 * 4. Loads `DSISETR` from the config struct (ECC, CRC mask, scramble,
 *    EoTp, tearing detect, max-return-packet size).
 * 5. Loads guard-band timing into `CLSTPTSETR / LPTRNSTSETR`.
 * 6. Loads bus timeouts into `HSTXTOSETR / LRXHTOSETR / TATOSETR /
 *    PRESPTOBTASETR / PRESPTOLPSETR / PRESPTOHSSETR`.
 * 7. Loads ULPS wake-up period into `ULPSSETR`.
 * 8. Clears all latched status flags so the first IRQ reflects only
 *    post-init events.
 *
 * The HS clock is **not** started by this function; call
 * `ra8_mipi_dsi_hs_clock_start()` separately so the application can
 * insert PHY-level steps in between.
 *
 * @param[in] cfg Static driver configuration. Must not be `nullptr`.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok                Driver initialized, link up.
 * @retval k_ra8_err_null_ptr      `cfg == nullptr`.
 * @retval k_ra8_err_invalid_arg   Lane count outside [1,2].
 * @retval k_ra8_err_hw_timeout    MSTP read-back loop timed out.
 *
 * @pre `cfg` is non-NULL.
 * @pre `cfg->lane_count` is one of the `k_ra8_mipi_dsi_lanes_*` values.
 * @post On success, `LINKSR` reports HSBUSY/LPBUSY clear.
 * @post On success, every sub-status register reads 0.
 *
 * @note Not thread-safe. Call from single-threaded init context.
 * @warning The MIPI PHY (HUM Ch 64) must be brought up first.
 *
 * @see ra8_mipi_dsi_deinit
 * @see ra8_mipi_dsi_hs_clock_start
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_init(const ra8_mipi_dsi_config_t* cfg);

/**
 * @brief Tear down the MIPI DSI host link layer.
 *
 * @details
 * Detaches any IRQ handler, asserts `RSTCR.SWRST`, then disables the
 * MSTP gate. Idempotent: calling twice in a row is harmless.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok                Block stopped, MSTP gated.
 * @retval k_ra8_err_hw_timeout    MSTP read-back loop timed out.
 *
 * @pre Driver was initialized with ::ra8_mipi_dsi_init (ignored if not).
 * @pre No active sequence operation (`LINKSR.SQ0RUN == SQ1RUN == 0`).
 * @post IRQ callback pointer cleared.
 * @post `MSTPCRC.MSTPC10` set (block clock gated).
 *
 * @note Not thread-safe.
 *
 * @see ra8_mipi_dsi_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_deinit(void);

/**
 * @brief Gate the MIPI DSI block at the MSTP register.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok               Block gated.
 * @retval k_ra8_err_hw_timeout   MSTP read-back loop timed out.
 *
 * @pre Sequence + video engines are idle (caller's responsibility).
 * @pre IRQs are masked.
 * @post `MSTPCRC.MSTPC10` is set.
 *
 * @see ra8_mipi_dsi_exit_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_enter_stop(void);

/**
 * @brief Re-enable the MIPI DSI block clock from MSTP.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok               Block clocked again.
 * @retval k_ra8_err_hw_timeout   MSTP read-back loop timed out.
 *
 * @pre Caller is in single-threaded context.
 * @pre PHY is up and out of LP-11 stop.
 * @post `MSTPCRC.MSTPC10` is clear, register I/O works again.
 *
 * @see ra8_mipi_dsi_enter_stop
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_exit_stop(void);

/**
 * @brief Pulse `RSTCR.SWRST` to take the protocol-layer state back to
 *        a clean slate (registers retained).
 *
 * @return `k_ra8_ok` always.
 *
 * @pre Caller has IRQs masked or is in IRQ context.
 * @pre Sequence channels are idle.
 * @post `RSTSR.RSTHS|RSTLP|RSTAPB|RSTAXI|RSTV` read 0 on success.
 * @post Block is ready for a fresh `ra8_mipi_dsi_hs_clock_start()`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_soft_reset(void);

/* =============================================================================
 * HS clock control
 * =============================================================================
 */

/**
 * @brief Start the high-speed differential clock to the panel.
 *
 * @details
 * Sets `HSCLKSETR.HSCLST = 1` and, if the init config selected
 * continuous mode, also sets `HSCLMD = 1`. Polls `PLSR.CLLP2HS` until
 * the clock-lane LP-to-HS event fires (bounded by
 * `k_ra8_mipi_dsi_busy_loop_max`).
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_hw_timeout` on poll timeout.
 *
 * @pre Driver is initialized.
 * @pre PHY is up.
 * @post `LINKSR.HSBUSY` clear once HS clock is stable.
 * @post Continuous mode: clock keeps running until
 *       `ra8_mipi_dsi_hs_clock_stop()`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_hs_clock_start(void);

/**
 * @brief Stop the high-speed differential clock.
 *
 * @details
 * Clears `HSCLKSETR.HSCLST` and waits for `PLSR.CLHS2LP`.
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_hw_timeout` on poll timeout.
 *
 * @pre Sequence channels idle.
 * @pre Video mode stopped.
 * @post HS clock lane is back to LP-11.
 * @post Subsequent HS Tx will fail until the clock is restarted.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_hs_clock_stop(void);

/* =============================================================================
 * Sequence-channel commands
 * =============================================================================
 */

/**
 * @brief Send a DSI short packet on sequence channel 0 (LP escape).
 *
 * @details
 * Backwards-compat helper -- assembles a default LP descriptor with
 * SPD=1, BTA=none, no ack, no aux, then pulses START. Equivalent to
 * `ra8_mipi_dsi_send_command()` with the matching `ra8_mipi_dsi_command_t`.
 *
 * @param[in] cmd_id One of the `k_ra8_mipi_dsi_dt_*` short-write constants.
 * @param[in] vc     Virtual channel (0..3).
 * @param[in] param0 First parameter byte.
 * @param[in] param1 Second parameter byte.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok                Packet queued and START asserted.
 * @retval k_ra8_err_invalid_arg   `vc > 3`.
 * @retval k_ra8_err_busy          Sequence engine already running.
 *
 * @pre Driver was initialized via ::ra8_mipi_dsi_init.
 * @pre `LINKSR.SQ0RUN` is 0.
 * @post Descriptor 0 of channel 0 carries the assembled header.
 * @post `SQCH0SET0R` was pulsed with START + CHSEL.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_short_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                       ra8_mipi_dsi_vc_t vc,
                                                       uint8_t           param0,
                                                       uint8_t           param1);

/**
 * @brief Send a DSI long-packet write (Generic Long / DCS Long).
 *
 * @details
 * Stages the payload bytes into TXPPD0..3R (max 16 bytes) and points
 * the descriptor at the packet RAM. For payloads > 16 bytes the
 * descriptor word D carries an external buffer pointer.
 *
 * @param[in] cmd_id   `k_ra8_mipi_dsi_dt_gen_long_write` or
 *                     `k_ra8_mipi_dsi_dt_dcs_long_write`.
 * @param[in] vc       Virtual channel (0..3).
 * @param[in] data     Payload bytes (`tx_len` long).
 * @param[in] tx_len   Payload length in bytes (max 65535 per spec).
 * @param[in] low_power true to send via channel 0 (LP), false for
 *                      channel 1 (HS).
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok               Packet queued, START asserted.
 * @retval k_ra8_err_null_ptr     `data == nullptr` and `tx_len > 0`.
 * @retval k_ra8_err_invalid_arg  `vc > 3` or LP and `tx_len > 128`.
 * @retval k_ra8_err_busy         Sequence engine already running.
 *
 * @pre Driver initialized.
 * @pre Either `tx_len == 0` or `data` valid.
 * @post Descriptor 0 of the selected channel carries the long-packet
 *       header (DATA0 = WC[7:0], DATA1 = WC[15:8]).
 * @post `SQCH*SET0R` pulsed with START + CHSEL for that channel.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_long_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                      ra8_mipi_dsi_vc_t vc,
                                                      const uint8_t*    data,
                                                      uint16_t          tx_len,
                                                      bool              low_power);

/**
 * @brief Send any sequence-channel command (full-feature path).
 *
 * @details
 * Assembles a descriptor from `cmd` -- handles short / long / aux-op /
 * BTA / ack-request / LP vs HS / read-with-rx-buffer in one entry
 * point. This is the FSP-equivalent surface; the helpers above just
 * wrap it.
 *
 * @param[in] cmd Validated command descriptor.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok                Packet queued.
 * @retval k_ra8_err_null_ptr      `cmd == nullptr`.
 * @retval k_ra8_err_invalid_arg   `cmd->virtual_channel > 3`, or LP and
 *                                `tx_len > 128`, or HS and `tx_len > 1024`,
 *                                or aux op while video running.
 * @retval k_ra8_err_busy          Sequence engine running.
 * @retval k_ra8_err_invalid_state LP requested while video mode running.
 *
 * @pre `cmd` non-NULL.
 * @pre Driver initialized.
 * @post Selected sequence channel descriptor 0 populated.
 * @post `SQCH*SET0R` pulsed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command(const ra8_mipi_dsi_command_t* cmd);

/**
 * @brief Issue a BTA-then-read on sequence channel 0 and return the
 *        captured response payload.
 *
 * @details
 * Builds a Generic / DCS read descriptor with `BTA = bta_read`, kicks
 * the sequence, then leaves response collection to the IRQ -- the
 * caller polls `ra8_mipi_dsi_rx_result_get()` for the slot containing
 * the result.
 *
 * @param[in]  cmd_id `k_ra8_mipi_dsi_dt_gen_read_*` or
 *                    `k_ra8_mipi_dsi_dt_dcs_read`.
 * @param[in]  vc     Virtual channel.
 * @param[in]  param0 First parameter byte.
 * @param[in]  param1 Second parameter byte.
 * @param[out] p_rx_buffer Buffer the IRQ handler will copy RXPPD into.
 * @param[in]  rx_len Maximum bytes the buffer can hold.
 *
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok               Read kicked off.
 * @retval k_ra8_err_null_ptr     `p_rx_buffer == nullptr`.
 * @retval k_ra8_err_invalid_arg  `vc > 3` or `rx_len == 0`.
 * @retval k_ra8_err_busy         Sequence engine running.
 *
 * @pre Driver initialized; PHY in BTA-capable state.
 * @pre `p_rx_buffer` lives until the IRQ fires.
 * @post Descriptor 0 of channel 0 holds a read header.
 * @post `SQCH0SET0R` pulsed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_read_packet(ra8_mipi_dsi_dt_t cmd_id,
                                                 ra8_mipi_dsi_vc_t vc,
                                                 uint8_t           param0,
                                                 uint8_t           param1,
                                                 uint8_t*          p_rx_buffer,
                                                 uint16_t          rx_len);

/* =============================================================================
 * ULPS
 * =============================================================================
 */

/**
 * @brief Drive the selected lanes into Ultra-Low-Power State.
 *
 * @param[in] lanes OR of `k_ra8_mipi_dsi_lane_clock` /
 *                  `k_ra8_mipi_dsi_lane_data`.
 *
 * @return `k_ra8_ok` on success; `k_ra8_err_invalid_arg` if `lanes == 0`
 *         or the clock lane is requested while continuous-clock mode
 *         is on (HW prohibits it).
 *
 * @pre Driver initialized and HS clock currently running.
 * @pre `lanes` is a non-empty subset of valid lane bits.
 * @post `ULPSCR.CLENT` and/or `DLENT` were pulsed for selected lanes.
 * @post Subsequent `ra8_mipi_dsi_send_*` is rejected until `_ulps_exit`.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_ulps_enter(uint8_t lanes);

/**
 * @brief Wake the selected lanes out of ULPS.
 *
 * @param[in] lanes OR of `k_ra8_mipi_dsi_lane_*` bits.
 *
 * @return `k_ra8_ok` on success; `k_ra8_err_invalid_arg` if `lanes == 0`.
 *
 * @pre At least one of the requested lanes is currently in ULPS
 *      (otherwise the exit pulse is a no-op).
 * @pre Driver initialized.
 * @post `ULPSCR.CLEXIT` and/or `DLEXIT` pulsed.
 * @post After the wake-up sequence completes (driver-tracked) the link
 *       is ready for HS / LP traffic again.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_ulps_exit(uint8_t lanes);

/* =============================================================================
 * Video mode
 * =============================================================================
 */

/**
 * @brief Programme every video-mode timing register from a single config.
 *
 * @details
 * Touches `VMSET1R`, `VMPPSETR`, `VMVSSETR`, `VMVPSETR`, `VMHSSETR`,
 * `VMHPSETR`. Does **not** assert `VMSET0R.VSTART` -- call
 * `ra8_mipi_dsi_video_start()` for that.
 *
 * @param[in] vcfg Pointer to video-mode timing struct.
 *
 * @return `k_ra8_ok` on success; `k_ra8_err_null_ptr` if `vcfg == nullptr`,
 *         `k_ra8_err_invalid_arg` if a field is out of range.
 *
 * @pre `vcfg` is non-NULL.
 * @pre All sync / porch / active-line counts fit into their HUM-defined
 *      bit widths.
 * @post Video timing registers loaded.
 * @post Subsequent `_video_start` will use these values.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_configure(const ra8_mipi_dsi_video_cfg_t* vcfg);

/**
 * @brief Start video-mode operation.
 *
 * @details
 * Writes `VMSET0R` with the HSA/HBP/HFP no-LP bits and the VSTART bit.
 * Polls `VMSR.VIRDY` until set (bounded).
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_hw_timeout` on poll timeout.
 *
 * @pre `_video_configure` has run since the last reset.
 * @pre HS clock running.
 * @post `LINKSR.VRUN` set.
 * @post Video pixel stream is being transmitted.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_start(const ra8_mipi_dsi_video_cfg_t* vcfg);

/**
 * @brief Request a video-mode stop and wait for it.
 *
 * @details
 * Sets `VMSET0R.VSTOP`, polls `VMSR.STOP` until 1, then scrubs the
 * VMSCR latches. Equivalent to FSP `dsi_stop`.
 *
 * @return `k_ra8_ok` on success, `k_ra8_err_hw_timeout` if VMSR.STOP
 *         never asserts within the bounded loop.
 *
 * @pre Video mode is currently running (otherwise call is a no-op).
 * @pre Driver initialized.
 * @post `LINKSR.VRUN` clear.
 * @post `VMSR` cleared of stale flags.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_video_stop(void);

/* =============================================================================
 * Status / IRQ
 * =============================================================================
 */

/**
 * @brief Read the top-level Interrupt Status Register.
 *
 * @param[out] out_mask Receives the raw ISR value.
 *
 * @return `k_ra8_ok` / `k_ra8_err_null_ptr`.
 *
 * @pre `out_mask` non-NULL.
 * @pre Driver init has run.
 * @post `*out_mask` reflects the current ISR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_get_status(uint32_t* out_mask);

/**
 * @brief Decode `LINKSR` into a struct.
 *
 * @param[out] out_status Decoded status.
 * @return `k_ra8_ok` / `k_ra8_err_null_ptr`.
 *
 * @pre `out_status` non-NULL.
 * @pre Driver initialized.
 * @post `*out_status` populated with the current LINKSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_link_status_get(ra8_mipi_dsi_link_status_t* out_status);

/**
 * @brief Clear bits in any of the sub-status registers.
 *
 * @details
 * Fans `mask` out across `SQCH0SCR`, `SQCH1SCR`, `VMSCR`, `RXSCR`,
 * `FERRSCR`, `PLSCR` so callers do not need to track which bit lives
 * where.
 *
 * @param[in] mask OR of `ra8_mipi_dsi_isr_mask_t` bits to clear.
 * @return `k_ra8_ok` always.
 *
 * @pre Driver init has run (or this is the bring-up path).
 * @pre `mask` only contains bits defined in `ra8_mipi_dsi_isr_mask_t`.
 * @post Cleared sub-status registers read 0 for the requested bits.
 * @post Other status bits are preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_clear_status(uint32_t mask);

/**
 * @brief Read the latest accumulated ack/error report and clear it.
 *
 * @param[out] out_err Decoded report.
 * @return `k_ra8_ok` / `k_ra8_err_null_ptr`.
 *
 * @pre `out_err` non-NULL.
 * @pre Driver initialized.
 * @post `AKEPACMSR` written to `AKEPSCR` (cleared in HW).
 * @post `*out_err` reflects the snapshot before clearing.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_ack_error_get(ra8_mipi_dsi_ack_error_t* out_err);

/**
 * @brief Decode and consume an RXRSS slot.
 *
 * @param[in]  slot      0..3 -- which RXRSS register.
 * @param[out] out_result Decoded packet header.
 * @return `k_ra8_ok` if the slot was valid, `k_ra8_err_no_data` if the
 *         slot's RXRSSR.SLT*VLD bit was clear.
 *
 * @pre `out_result` non-NULL.
 * @pre `slot` in 0..3.
 * @post On success, slot's RXRSSR + RXRINFOOWSR valid bits cleared.
 * @post `*out_result` carries the decoded header.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_rx_result_get(uint8_t                   slot,
                                                   ra8_mipi_dsi_rx_result_t* out_result);

/**
 * @brief Copy out the most-recent long-packet RX payload (RXPPD0..3R).
 *
 * @param[out] dest    Caller buffer.
 * @param[in]  max_len Bytes available in `dest` (capped at 16).
 * @param[out] out_len Bytes actually read.
 * @return `k_ra8_ok` / `k_ra8_err_null_ptr`.
 *
 * @pre `dest` and `out_len` non-NULL.
 * @pre `max_len <= k_ra8_mipi_dsi_payload_max`.
 * @post `dest[0..*out_len)` populated.
 * @post `*out_len` <= 16.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mipi_dsi_rx_payload_read(uint8_t* dest, uint16_t max_len, uint16_t* out_len);

/**
 * @brief Check if a tearing-effect (TE) trigger was received.
 *
 * @param[out] out_pending Receives true if `RXSR.RXTE` (rx TE trigger)
 *                         or `RXSR.EXTEDET` (external TE pin) is set.
 * @return `k_ra8_ok` / `k_ra8_err_null_ptr`.
 *
 * @pre `out_pending` non-NULL.
 * @pre Driver initialized.
 * @post `*out_pending` reflects current RXSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_te_event_pending(bool* out_pending);

/**
 * @brief Clear the latched tearing-effect bits in RXSR.
 *
 * @return `k_ra8_ok`.
 *
 * @pre Driver initialized.
 * @pre Caller is in IRQ context or has IRQs masked.
 * @post `RXSR.RXTE / EXTEDET` clear.
 * @post Other RXSR bits preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_te_event_clear(void);

/**
 * @brief Toggle a sub-class IER bit.
 *
 * @param[in] event  Which class (`k_ra8_mipi_dsi_event_*`).
 * @param[in] mask   Bits inside the class IER register to update.
 * @param[in] enable true -> set selected bits, false -> clear.
 *
 * @return `k_ra8_ok` / `k_ra8_err_invalid_arg` for unknown class.
 *
 * @pre Driver initialized.
 * @pre `event` is a defined enumerator.
 * @post Selected IER bits updated.
 * @post Other IER bits preserved.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mipi_dsi_irq_enable(ra8_mipi_dsi_event_t event, uint32_t mask, bool enable);

/**
 * @brief Attach a callback for MIPI DSI interrupt events.
 *
 * @param[in] fn  Function to call (or `nullptr` to detach).
 * @param[in] ctx Opaque context passed back to `fn`.
 *
 * @return `k_ra8_ok`.
 *
 * @pre Caller's `fn` is ISR-safe.
 * @pre Caller owns `ctx` lifetime for as long as `fn` is attached.
 * @post Subsequent `ra8_mipi_dsi_dispatch*` calls invoke `fn`.
 * @post Detached state silently swallowed.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_attach_handler(ra8_mipi_dsi_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot ISR, clear sub-status, fan out per-class callbacks.
 *
 * @details
 * Convenience trampoline that calls every per-class
 * `ra8_mipi_dsi_dispatch_*` whose ISR bit is set. The platform's NVIC
 * stub may also call individual `_dispatch_*` directly when wired up
 * per-IRQ.
 *
 * @pre Called either in IRQ context or with IRQs masked.
 * @pre Driver init has run.
 * @post `ISR` reads 0 for the bits that were set when the call began.
 * @post Per-class dispatch trampolines have each fired at most once.
 *
 * @note Not thread-safe; pair with NVIC masking. See HUM Ch 65
 *       "MIPI DSI Host" pp 3839-3934.
 * @since 0.1.0
 */
void ra8_mipi_dsi_dispatch(void);

/** @brief Sequence-channel-0 IRQ handler (LP path).
 *  @details Snapshots SQCH0SR, clears the latched bits and invokes the
 *           registered seq0 callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `SQCH0SR` cleared. @post Callback invoked with seq0 event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_seq0(void);

/** @brief Sequence-channel-1 IRQ handler (HS path).
 *  @details Snapshots SQCH1SR, clears the latched bits and invokes the
 *           registered seq1 callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `SQCH1SR` cleared. @post Callback invoked with seq1 event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_seq1(void);

/** @brief Video-mode IRQ handler.
 *  @details Snapshots VMSR, clears the latched bits and invokes the
 *           registered video callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `VMSR` cleared. @post Callback invoked with video event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_video(void);

/** @brief Receive IRQ handler.
 *  @details Snapshots RXSR, clears the latched bits and invokes the
 *           registered receive callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `RXSR` cleared. @post Callback invoked with receive event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_receive(void);

/** @brief Fatal-error IRQ handler.
 *  @details Snapshots FERRSR, clears the latched bits and invokes the
 *           registered fatal callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `FERRSR` cleared. @post Callback invoked with fatal event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_fatal(void);

/** @brief PHY-protocol-interface IRQ handler.
 *  @details Snapshots PLSR, clears the latched bits and invokes the
 *           registered phy callback. Safe to call from a spurious IRQ.
 *  @pre Driver init done. @pre IRQ context.
 *  @post `PLSR` cleared. @post Callback invoked with phy event.
 *  @note ISR-context only; not thread-safe.
 *  @since 0.1.0 */
void ra8_mipi_dsi_dispatch_phy(void);

/* =============================================================================
 * Sweep 6 convenience surfaces
 * =============================================================================
 */

/**
 * @struct ra8_mipi_dsi_video_timing_t
 * @brief Compact video-mode timing block (HSA/HBP/HACT/HFP + VSA/VBP/VACT/VFP).
 *
 * @details
 * Lighter-weight cousin of ::ra8_mipi_dsi_video_cfg_t for callers that just
 * need to load timing numbers and accept the driver defaults for sync
 * polarity, pixel format (RGB888) and HSA/HBP/HFP-no-LP behaviour.
 * Maps onto VMVSSETR / VMVPSETR / VMHSSETR / VMHPSETR per HUM Ch 65
 * "MIPI DSI Host" pp 3839-3934.
 *
 * @invariant All eight counts fit into the bit widths of their target
 *            registers (sync = 12 bits, porch = 13 bits, active = 15 bits).
 *
 * @code
 * const ra8_mipi_dsi_video_timing_t t = {
 *   .horizontal_sync   = 12,  .horizontal_back_porch = 64,
 *   .horizontal_active = 1024, .horizontal_front_porch = 32,
 *   .vertical_sync     = 4,   .vertical_back_porch    = 8,
 *   .vertical_active   = 600, .vertical_front_porch   = 6,
 * };
 * (void)ra8_mipi_dsi_set_video_timing(&t);
 * @endcode
 *
 * @see ra8_mipi_dsi_video_cfg_t for the full (polarity-aware) struct.
 *
 * @since 0.1.0
 */
typedef struct {
  uint16_t horizontal_sync;        /**< HSA pixel count -> VMHSSETR.HSA.   */
  uint16_t horizontal_back_porch;  /**< HBP pixel count -> VMHPSETR.HBP.   */
  uint16_t horizontal_active;      /**< HACT pixel count -> VMHSSETR.HACT. */
  uint16_t horizontal_front_porch; /**< HFP pixel count -> VMHPSETR.HFP.   */
  uint16_t vertical_sync;          /**< VSA line count  -> VMVSSETR.VSA.   */
  uint16_t vertical_back_porch;    /**< VBP line count  -> VMVPSETR.VBP.   */
  uint16_t vertical_active;        /**< VACT line count -> VMVSSETR.VACT.  */
  uint16_t vertical_front_porch;   /**< VFP line count  -> VMVPSETR.VFP.   */
} ra8_mipi_dsi_video_timing_t;

/**
 * @brief Load only the HSA/HBP/HACT/HFP/VSA/VBP/VACT/VFP video-timing
 *        registers from a compact descriptor.
 *
 * @details
 * Wraps ::ra8_mipi_dsi_video_configure with sensible defaults
 * (RGB888 pixel format on VC0, sync-pulse mode off, HSA/HBP/HFP-no-LP
 * all set so the link stays HS through blanking). The remaining
 * VMSET1R / VMPPSETR fields keep their power-on defaults.
 *
 * @param[in] timing Compact timing descriptor.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Timing registers loaded.
 * @retval k_ra8_err_null_ptr     `timing == nullptr`.
 * @retval k_ra8_err_invalid_arg  A field is wider than the target register.
 *
 * @pre Driver initialized.
 * @pre `timing` is non-NULL.
 * @post Video-mode timing registers reflect the new values.
 * @post Other VM* registers preserve their power-on defaults.
 *
 * @note Not thread-safe. Call from single-threaded init or with IRQs masked.
 * @see ra8_mipi_dsi_video_configure -- full-feature surface.
 * @see ra8_mipi_dsi_video_start
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_set_video_timing(const ra8_mipi_dsi_video_timing_t* timing);

/**
 * @brief Send a DSI command-mode short packet on VC0 (LP escape).
 *
 * @details
 * Convenience surface around ::ra8_mipi_dsi_send_short_packet that
 * matches the FSP "command short" signature: a Data Type plus a
 * 2-byte parameter array. Always sent on virtual channel 0 in low
 * power escape mode. HUM Ch 65 "Command-mode short packet" p 3839+.
 *
 * @param[in] dt     One of the `k_ra8_mipi_dsi_dt_*_short_*` constants.
 * @param[in] params 2-element array carrying parameter 0 / 1.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok                Packet queued.
 * @retval k_ra8_err_null_ptr      `params == nullptr`.
 * @retval k_ra8_err_busy          Sequence engine still running.
 * @retval k_ra8_err_invalid_state Video mode running (LP rejected).
 *
 * @pre Driver initialized.
 * @pre `params` is non-NULL.
 * @post Sequence channel 0 descriptor 0 carries the assembled header.
 * @post `SQCH0SET0R` pulsed with START.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command_short(ra8_mipi_dsi_dt_t dt,
                                                        const uint8_t     params[2]);

/**
 * @brief Send a DSI command-mode long packet on VC0 (HS path).
 *
 * @details
 * Convenience surface around ::ra8_mipi_dsi_send_long_packet -- always
 * uses VC0 and the high-speed sequence channel 1, which is what every
 * command-mode panel programming sequence wants. Use the lower-level
 * helper if you need LP routing or a non-zero VC. HUM Ch 65 "Long
 * packet write" p 3839+.
 *
 * @param[in] dt      `k_ra8_mipi_dsi_dt_gen_long_write` or
 *                    `k_ra8_mipi_dsi_dt_dcs_long_write`.
 * @param[in] payload Payload bytes (`len` bytes long).
 * @param[in] len     Payload length in bytes (max 1024 in HS mode).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Packet queued, START asserted.
 * @retval k_ra8_err_null_ptr     `payload == nullptr` and `len > 0`.
 * @retval k_ra8_err_invalid_arg  `len > 1024`.
 * @retval k_ra8_err_busy         Sequence engine running.
 *
 * @pre Driver initialized.
 * @pre Either `len == 0` or `payload` valid.
 * @post Descriptor 0 of channel 1 carries the long-packet header.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_mipi_dsi_send_command_long(ra8_mipi_dsi_dt_t dt, const uint8_t* payload, uint16_t len);

/**
 * @brief Send a DSI command-mode packet (short or long) on VC0 in LP escape.
 *
 * @details
 * Routes a command-mode payload through the LP-escape sequence
 * channel.  Short writes (``len <= 2``) use the short-packet path;
 * longer writes go through the long-packet path with the payload
 * staged into TXPPD0..3R.  Mirrors HUM Ch 65 "Command-mode packet
 * TX" pp 3839-3934.
 *
 * @param[in] packet_type DSI Data Type (see ``k_ra8_mipi_dsi_dt_*``).
 * @param[in] payload     Payload bytes (``len`` bytes).
 * @param[in] len         Payload length in bytes (LP cap = 128).
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok               Packet queued.
 * @retval k_ra8_err_null_ptr     ``payload == nullptr`` and ``len > 0``.
 * @retval k_ra8_err_invalid_arg  ``len > 128`` (LP cap).
 * @retval k_ra8_err_busy         Sequence engine still running.
 *
 * @pre Driver initialized via ::ra8_mipi_dsi_init.
 * @pre Either ``len == 0`` or ``payload`` is non-NULL.
 * @post Sequence channel 0 descriptor 0 carries the assembled header.
 * @post ``SQCH0SET0R`` pulsed with START.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_send_command_payload(ra8_mipi_dsi_dt_t packet_type,
                                                          const uint8_t*    payload,
                                                          uint16_t          len);

/**
 * @brief Drive the entire link (clock + data lanes) into ULPS.
 *
 * @details
 * Convenience equivalent to ``ra8_mipi_dsi_ulps_enter(k_ra8_mipi_dsi_lane_all)``.
 * Honours the same continuous-clock-mode guard as the underlying call.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok              Both lanes pulsed into ULPS.
 * @retval k_ra8_err_invalid_arg Continuous-clock mode active and clock
 *                              lane requested -- HW prohibits it.
 *
 * @pre Driver initialized, HS clock currently running.
 * @pre Continuous-clock mode is OFF.
 * @post ULPSCR.CLENT and DLENT have been pulsed.
 * @post Subsequent `ra8_mipi_dsi_send_*` is rejected until ::ra8_mipi_dsi_exit_ulps.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_enter_ulps(void);

/**
 * @brief Wake the entire link out of ULPS.
 *
 * @details
 * Convenience equivalent to ``ra8_mipi_dsi_ulps_exit(k_ra8_mipi_dsi_lane_all)``.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok Both lanes woken.
 *
 * @pre At least one of the lanes was previously placed in ULPS.
 * @pre Driver initialized.
 * @post ULPSCR.CLEXIT and DLEXIT pulsed.
 * @post Link is ready for HS / LP traffic again once the wake-up
 *       sequence completes.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_exit_ulps(void);

/**
 * @brief Snapshot the link state (running flags + busy flags + accumulated
 *        ack/error report) into a single struct.
 *
 * @details
 * Convenience around ::ra8_mipi_dsi_link_status_get -- some callers want
 * the more direct "get_link_status" name from the FSP surface.
 *
 * @param[out] out Decoded link status.
 *
 * @return ::ra8_err_t outcome.
 * @retval k_ra8_ok           Status returned.
 * @retval k_ra8_err_null_ptr `out == nullptr`.
 *
 * @pre `out` non-NULL.
 * @pre Driver initialized.
 * @post `*out` reflects the current LINKSR snapshot.
 * @post No registers mutated.
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_mipi_dsi_get_link_status(ra8_mipi_dsi_link_status_t* out);

#ifdef __cplusplus
}
#endif
