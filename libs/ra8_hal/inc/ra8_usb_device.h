/**
 * @file ra8_usb_device.h
 * @brief Native USB controller driver public API -- shared types + device mode
 * @ingroup grp_hal_usb
 *
 * @details
 * Device-mode half of the hand-written, FSP-equivalent driver for the
 * two USB controllers on the Renesas RA8D2 (USBFS @ 0x40250000,
 * USBHS @ 0x40351000). This sub-header carries the public types shared
 * by both device and host modes plus the device-mode lifecycle, status,
 * endpoint, control-transfer, IRQ-delivery, and power surface. The
 * host-mode surface lives in `ra8_usb_host.h`. Both are aggregated by the
 * thin umbrella `ra8_usb.h`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 * @since 0.1.0
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra8_usb_speed_t
 * @brief Selects which controller instance the call targets.
 */
typedef enum : uint8_t {
  k_ra8_usb_speed_fs = 0U, /**< Full-Speed controller (USBFS @ 0x40250000). */
  k_ra8_usb_speed_hs = 1U, /**< High-Speed controller (USBHS @ 0x40351000). */
} ra8_usb_speed_t;

/**
 * @enum ra8_usb_dev_state_t
 * @brief Decoded `INTSTS0.DVSQ[2:0]` device-state values.
 *
 * @details Stable enum used by the public surface. Internal masks
 * still live in `ra8_usb_regs.h::ra8_usb_dvsq_t`.
 */
typedef enum : uint8_t {
  k_ra8_usb_dev_state_powered    = 0U, /**< Powered, no reset yet. */
  k_ra8_usb_dev_state_default    = 1U, /**< Default (post reset).  */
  k_ra8_usb_dev_state_address    = 2U, /**< Address assigned.      */
  k_ra8_usb_dev_state_configured = 3U, /**< Configured.            */
  k_ra8_usb_dev_state_suspended  = 4U, /**< Suspended.             */
} ra8_usb_dev_state_t;

/**
 * @enum ra8_usb_ep_dir_t
 * @brief Endpoint direction.
 */
typedef enum : uint8_t {
  k_ra8_usb_ep_dir_out = 0U, /**< Host -> device. */
  k_ra8_usb_ep_dir_in  = 1U, /**< Device -> host. */
} ra8_usb_ep_dir_t;

/**
 * @enum ra8_usb_ep_type_t
 * @brief Transfer type for a non-control endpoint.
 */
typedef enum : uint8_t {
  k_ra8_usb_ep_type_bulk = 0U, /**< Bulk transfer.        */
  k_ra8_usb_ep_type_intr = 1U, /**< Interrupt transfer.   */
  k_ra8_usb_ep_type_iso  = 2U, /**< Isochronous transfer. */
} ra8_usb_ep_type_t;

/**
 * @struct ra8_usb_setup_t
 * @brief Decoded 8-byte USB SETUP packet.
 *
 * @details Mirrors USB 2.0 chapter 9 layout. Populated by
 * `ra8_usb_read_setup_if_valid` / `ra8_usb_read_setup_unconditional`
 * from the controller's USBREQ / USBVAL /
 * USBINDX / USBLENG mirror registers.
 */
typedef struct {
  uint8_t  bm_request_type; /**< Request direction / type / recipient. */
  uint8_t  b_request;       /**< bRequest code.                        */
  uint16_t w_value;         /**< wValue.                               */
  uint16_t w_index;         /**< wIndex.                               */
  uint16_t w_length;        /**< wLength.                              */
} ra8_usb_setup_t;

/**
 * @typedef ra8_usb_event_fn_t
 * @brief USB event callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] speed Which controller fired.
 * @param[in] status_mask Snapshot of `INTSTS0` at dispatch time.
 *
 * @note Invoked from the dispatch site (typically ISR context).
 */
typedef void (*ra8_usb_event_fn_t)(void* ctx, ra8_usb_speed_t speed, uint16_t status_mask);

/* =============================================================================
 * Lifecycle
 * =============================================================================
 */

/**
 * @brief Bring up a USB controller in device mode.
 *
 * @details
 * Mirrors FSP's `hw_usb_pmodule_init` for the chosen instance:
 *
 *  1. Releases the controller's MSTP gate.
 *  2. Drives `SYSCFG.SCKE` high and waits until the clock is stable.
 *  3. Clears `SYSCFG.DRPD` (host pull-down) and sets `SYSCFG.USBE`.
 *  4. Sets `SYSCFG.HSE` for the HS instance only.
 *  5. Programs the C/D0/D1 FIFOSEL access width to 16-bit.
 *  6. Loads the default control pipe (DCP) max-packet size to 64.
 *  7. Enables the device-mode interrupt set
 *     (BEMPE | BRDYE | NRDYE | DVSE | CTRE | VBSE).
 *
 * D+ pull-up stays off; the caller raises it via
 * `ra8_usb_device_attach` once descriptors are wired up.
 *
 * @param[in] speed Which controller to bring up.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Controller ready, D+ pull-up off.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed MSTP release failed.
 *
 * @pre Caller holds single-threaded init context (or IRQs masked).
 * @pre `ra8_mstp_init` and `ra8_pwr_init` have run.
 *
 * @post Controller is clocked, D+ pull-up off, IRQs unmasked at
 * controller level (NVIC line still owned by `ra8_irq`).
 * @post `ra8_usb_get_device_state` returns
 * `k_ra8_usb_dev_state_powered`.
 *
 * @note Not thread-safe.
 * @see ra8_usb_device_attach
 * @see ra8_usb_device_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down a USB controller and drop its MSTP reference.
 *
 * @param[in] speed Which controller to release.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Controller released.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `SYSCFG`, `INTENB0`, `INTENB1` all zero.
 * @post Controller MSTP-gated.
 *
 * @note Not thread-safe.
 * @see ra8_usb_device_init
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_deinit(ra8_usb_speed_t speed);

/**
 * @brief Raise / drop the D+ pull-up to advertise the device to the host.
 *
 * @param[in] speed Which controller.
 * @param[in] attached `true` to assert pull-up, `false` to drop.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pull-up state updated.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre `ra8_usb_device_init` has been called for this `speed`.
 *
 * @post On success, `SYSCFG.DPRPU` matches `attached`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_attach(ra8_usb_speed_t speed, bool attached);

/* =============================================================================
 * Status / state
 * =============================================================================
 */

/**
 * @brief Read raw `INTSTS0` snapshot.
 *
 * @param[in] speed Which controller.
 * @param[out] out_mask Receives the current `INTSTS0`.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Mask returned.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_null_ptr `out_mask` was NULL.
 *
 * @pre `out_mask` non-NULL.
 *
 * @post No controller state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_get_status(ra8_usb_speed_t speed, uint16_t* out_mask);

/**
 * @brief Clear bits in `INTSTS0`.
 *
 * @param[in] speed Which controller.
 * @param[in] mask Bits to clear.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bits cleared.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post `INTSTS0 & mask == 0`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_clear_status(ra8_usb_speed_t speed, uint16_t mask);

/**
 * @brief Read the decoded device state from `INTSTS0.DVSQ[2:0]`.
 *
 * @param[in] speed Which controller.
 * @param[out] out_state Receives the decoded state.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok State read.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_null_ptr `out_state` was NULL.
 *
 * @pre `out_state` non-NULL.
 *
 * @post No controller state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_get_device_state(ra8_usb_speed_t      speed,
                                                 ra8_usb_dev_state_t* out_state);

/**
 * @brief Program the device USB address into `USBADDR`.
 *
 * @details
 * Called by the chapter-9 stack after a successful SET_ADDRESS
 * SETUP completion. The controller answers SET_ADDRESS automatically
 * before the status stage; this helper only stores the address so
 * the host's subsequent IN tokens land on the right device.
 *
 * @param[in] speed Which controller.
 * @param[in] address USB address (0..127).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Address written.
 * @retval k_ra8_err_invalid_arg `speed` out of range or address > 127.
 *
 * @pre Bus is in default state (post reset, pre SET_ADDRESS).
 *
 * @post `USBADDR.USBADDR[6:0] == address`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_set_address(ra8_usb_speed_t speed, uint8_t address);

/**
 * @brief Re-arm the default control pipe (DCP) after a host-issued bus reset.
 *
 * @details
 * The RA8D2 USB IP latches `INTSTS0.DVST` whenever the host drives a
 * USB bus reset that puts the controller into the Default state
 * (`DVSQ == 0x10`). FSP's reference flow (`usb_pstd_busreset` in
 * `r_usb_basic/src/driver/r_usb_psignal.c`) re-programmes `DCPCFG = 0`
 * and `DCPMAXP = max_packet` on every such transition so the IP is
 * ready to ACK the host's first SETUP token within the 10 ms window
 * USB 2.0 Section 9.2.6.3 mandates between reset-deassert and the
 * first GET_DESCRIPTOR. Without this re-arm the controller silently
 * drops SETUP tokens, the host re-issues bus reset, and the device
 * loops forever between Default and Suspended-from-Default
 * (`DVSQ = 0x10` -> `0x50`).
 *
 * Specifically this call:
 *  1. Re-writes `DCPCFG = 0` (no continuous transfers).
 *  2. Re-writes `DCPMAXP = 64` (HS / FS bMaxPacketSize0).
 *  3. Clears `DCPCTR` so PID returns to NAK and CCPL is de-asserted.
 *  4. Walks `PIPECTR[1..9]` clearing PID + ACLRM so any pre-reset
 *     pipe state is forgotten.
 *  5. Re-applies the device-mode `INTENB0` mask (BEMP / BRDY / NRDY /
 *     CTRT / DVST / SOFR / RSME / VBSE) -- the IP can drop CTRT
 *     enables across a bus reset on some silicon revisions.
 *
 * @param[in] speed Which controller (`k_ra8_usb_speed_fs` or `_hs`).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok DCP re-armed.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 * @pre Caller observed a `DVST` interrupt with `DVSQ == default`.
 *
 * @post `DCPMAXP == 64`, `DCPCTR == 0`, all `PIPECTR[i]` PID == NAK.
 * @post `INTENB0` matches the post-init device-mode mask.
 *
 * @note Safe to call from IRQ-callback context.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_device_busreset_rearm(ra8_usb_speed_t speed);

/* =============================================================================
 * Endpoints
 * =============================================================================
 */

/**
 * @brief Configure a non-control PIPE for IN or OUT bulk / interrupt /
 * iso transfers.
 *
 * @details
 * Programs `PIPESEL`, `PIPECFG`, `PIPEMAXP`, `PIPECTR[n]` for
 * `pipe_num`. Mirrors FSP's `usb_cstd_pipe_table` writes condensed
 * for the device-mode case. The pipe is left with PID = NAK so the
 * stack can queue data with `ra8_usb_queue_in` /
 * `ra8_usb_queue_out` before transitioning to BUF.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 1..9.
 * @param[in] ep_addr Endpoint address (USB EP number, 1..15).
 * @param[in] dir Endpoint direction.
 * @param[in] type Endpoint type.
 * @param[in] max_packet Maximum packet size.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Pipe configured.
 * @retval k_ra8_err_invalid_arg Pipe / EP / dir / type / size invalid.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 *
 * @post Pipe responds NAK, software toggle cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_configure_endpoint(ra8_usb_speed_t   speed,
                                                   uint8_t           pipe_num,
                                                   uint8_t           ep_addr,
                                                   ra8_usb_ep_dir_t  dir,
                                                   ra8_usb_ep_type_t type,
                                                   uint16_t          max_packet);

/**
 * @brief Stall a configured endpoint.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 0..9 (0 = DCP).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok PID set to STALL.
 * @retval k_ra8_err_invalid_arg Pipe out of range.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 *
 * @post Pipe responds STALL until the stack clears the halt.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_stall_endpoint(ra8_usb_speed_t speed, uint8_t pipe_num);

/**
 * @brief Queue an IN transfer (device -> host) on `pipe_num`.
 *
 * @details
 * Selects `pipe_num` on `CFIFOSEL`, waits for `FRDY`, writes
 * `data[0..len-1]` 16-bit-aligned, asserts `BVAL`, and switches the
 * pipe PID to BUF so the controller hands the buffer to the SIE on
 * the next IN token. Short / zero-length packets are handled
 * naturally because `BVAL` is asserted regardless of length.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 1..9.
 * @param[in] data Buffer to transmit (NULL allowed iff len == 0).
 * @param[in] len Transmit byte count, 0..max_packet of the pipe.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Transfer queued.
 * @retval k_ra8_err_invalid_arg Pipe / len / data combination invalid.
 * @retval k_ra8_err_hw_timeout `FRDY` never asserted.
 *
 * @pre Pipe previously configured via
 * `ra8_usb_configure_endpoint(..., dir = IN, ...)`.
 *
 * @post Pipe PID set to BUF.
 * @post `BVAL` asserted on the FIFO port.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_queue_in(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len);

/**
 * @brief Drain an OUT transfer (host -> device) from `pipe_num`.
 *
 * @details
 * Counterpart to `ra8_usb_queue_in`. Selects `pipe_num`, waits for
 * `FRDY`, and reads up to `*inout_len` bytes from the FIFO. When
 * `rearm` is true the pipe PID is set back to BUF so the controller
 * ACKs the next host OUT token; when false the PID is left untouched
 * so the caller owns the arm/park decision -- re-arming would re-open
 * the pipe for a window in which a stray host OUT packet could land
 * with no receiver and storm the ISR.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 1..9.
 * @param[out] out_buf Receive buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes read.
 * @param[in] rearm `true` re-arms PID=BUF after the drain; `false`
 *                  leaves the PID untouched (caller arms or parks).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes read.
 * @retval k_ra8_err_no_data Buffer was empty.
 * @retval k_ra8_err_invalid_arg Pipe / pointers invalid.
 * @retval k_ra8_err_hw_timeout `FRDY` never asserted.
 *
 * @pre Pipe previously configured for OUT.
 * @pre `out_buf`, `inout_len` non-NULL, `*inout_len > 0`.
 *
 * @post Pipe PID is BUF if `rearm`, otherwise left unchanged.
 * @post `*inout_len` reflects actual byte count delivered.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_queue_out(ra8_usb_speed_t speed,
                                          uint8_t         pipe_num,
                                          uint8_t*        out_buf,
                                          uint16_t*       inout_len,
                                          bool            rearm);

/**
 * @brief Re-arm an OUT pipe that the controller has parked at PID=NAK.
 *
 * @details
 * The RA8D2 USB-FS / USB-HS pipe state machine auto-flips PID from BUF
 * to NAK after every successful BUF cycle on a single-buffered pipe
 * (HUM Ch 36 -- single-buffered OUT pipes). `ra8_usb_queue_out` leaves
 * the PID untouched after a drain, so the OUT pipe sits at NAK and the
 * controller NAKs incoming transactions (NRDYSTS accumulates the NAK
 * responses). This helper clears the per-pipe NRDYSTS bit (W0C) and
 * unconditionally re-asserts PID=BUF so the pipe is ready to ACK the
 * next host OUT token. Safe to call
 * proactively from the polled-dispatch worker on every iteration for
 * each OUT pipe that has a pending USBX transfer.
 *
 * @param[in] speed    Which controller (FS / HS).
 * @param[in] pipe_num PIPE1..PIPE9 (must not be 0; DCP uses DCPCTR).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok               Pipe re-armed at PID=BUF, NRDYSTS bit acked.
 * @retval k_ra8_err_invalid_arg  `speed` invalid or `pipe_num` out of range.
 *
 * @pre Pipe was previously configured via `ra8_usb_configure_endpoint`
 *      with `dir = k_ra8_usb_ep_dir_out`.
 * @pre Caller serialises against `ra8_usb_queue_out` for the same pipe.
 *
 * @post `NRDYSTS` bit `pipe_num` is cleared.
 * @post `PIPECTR[pipe_num-1].PID == BUF`.
 *
 * @note Not thread-safe; the polled-dispatch worker is the sole caller
 *       on hardware.
 *
 * @see ra8_usb_queue_out
 * @see ra8_usb_configure_endpoint
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_rearm_out_pipe(ra8_usb_speed_t speed, uint8_t pipe_num);

/**
 * @brief Park an OUT pipe at PID=NAK so an idle pipe stays quiescent.
 *
 * @details
 * The inverse of ::ra8_usb_rearm_out_pipe. An OUT pipe left at PID=BUF
 * with its per-pipe BRDY enabled but with no consumer ready will ACK
 * the next host OUT token into the pipe FIFO, latch BRDYSTS, and --
 * because nothing drains it -- hold INTSTS0.BRDY asserted, which
 * re-fires the USB ISR continuously and starves RTOS thread mode.
 * Parking the pipe at PID=NAK makes the controller NAK host OUT
 * tokens (normal USB flow control: the host simply retries), so no
 * data enters the FIFO, BRDYSTS never latches, and the ISR stays
 * quiet until a consumer arms the pipe with ::ra8_usb_rearm_out_pipe.
 *
 * @param[in] speed    Which controller (FS / HS).
 * @param[in] pipe_num PIPE1..PIPE9 (must not be 0; DCP uses DCPCTR).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok               Pipe parked at PID=NAK.
 * @retval k_ra8_err_invalid_arg  `speed` invalid or `pipe_num` out of range.
 *
 * @pre Pipe was previously configured via `ra8_usb_configure_endpoint`
 *      with `dir = k_ra8_usb_ep_dir_out`.
 * @pre No transfer is currently in flight on the pipe.
 *
 * @post `PIPECTR[pipe_num-1].PID == NAK`.
 *
 * @note Not thread-safe; caller serialises against `ra8_usb_rearm_out_pipe`.
 *
 * @see ra8_usb_rearm_out_pipe
 * @see ra8_usb_configure_endpoint
 *
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_park_out_pipe(ra8_usb_speed_t speed, uint8_t pipe_num);

/* =============================================================================
 * Control transfers (EP0 / DCP)
 * =============================================================================
 */

/**
 * @brief Snapshot the current SETUP packet from the controller, gated
 *        on `INTSTS0.VALID` (FS / CTRT path).
 *
 * @details
 * Reads the four mirror registers `USBREQ`, `USBVAL`, `USBINDX`,
 * `USBLENG` and clears `INTSTS0.VALID`. The function returns
 * `k_ra8_err_no_data` when `VALID == 0` so the FS / CTRT-driven path
 * can poll the same routine without producing stale dispatches. Use
 * ::ra8_usb_read_setup_unconditional on the HS / SQMON polled-worker
 * path where the SIE may auto-clear `VALID` before the worker observes
 * the SETUP edge.
 *
 * @param[in] speed Which controller.
 * @param[out] out_setup Decoded 8-byte SETUP packet.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP captured.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_null_ptr `out_setup` was NULL.
 * @retval k_ra8_err_no_data `INTSTS0.VALID` was clear.
 *
 * @pre `out_setup` non-NULL.
 *
 * @post `INTSTS0.VALID` cleared on success.
 *
 * @note Call from `CTRT`-handling path. HS dispatcher must use
 *       ::ra8_usb_read_setup_unconditional instead.
 *
 * @see ra8_usb_read_setup_unconditional
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_read_setup_if_valid(ra8_usb_speed_t  speed,
                                                    ra8_usb_setup_t* out_setup);

/**
 * @brief Snapshot the current SETUP packet from the controller without
 *        gating on `INTSTS0.VALID` (HS / SQMON polled-worker path).
 *
 * @details
 * On HS the polled SETUP worker observes `DCPCTR.SQMON == 1` (HUM
 * Ch 37.2.31 p 2095, race-immune SETUP-latched signal) AFTER the SIE
 * has already auto-cleared `INTSTS0.VALID`. The captured SETUP latch
 * registers `USBREQ` / `USBVAL` / `USBINDX` / `USBLENG`
 * (HUM Ch 37.2.21..24 p 2087..2090) survive that auto-clear -- only a
 * fresh SETUP token can overwrite them. This entry point therefore
 * skips the `VALID` gate and drains the captured registers directly,
 * then defensively W0C-clears `VALID` (no-op if already 0).
 *
 * @param[in] speed Which controller.
 * @param[out] out_setup Decoded 8-byte SETUP packet.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP drained from captured registers.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_null_ptr `out_setup` was NULL.
 *
 * @pre `out_setup` non-NULL.
 * @pre Caller has independent proof a SETUP arrived (e.g. SQMON==1).
 *
 * @post `*out_setup` mirrors USBREQ/USBVAL/USBINDX/USBLENG.
 * @post `INTSTS0.VALID` is W0C-cleared.
 *
 * @note Call from the HS / SQMON polled-dispatcher path.
 *
 * @see ra8_usb_read_setup_if_valid
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_read_setup_unconditional(ra8_usb_speed_t  speed,
                                                         ra8_usb_setup_t* out_setup);

/**
 * @brief Issue a control-transfer status response on EP0.
 *
 * @details
 * Wraps the `DCPCTR.PID` + `DCPCTR.CCPL` dance documented in HUM
 * Ch 36.2.21. Pass `accept = true` to drive ACK / move to the status
 * stage; `accept = false` issues STALL on EP0.
 *
 * @param[in] speed Which controller.
 * @param[in] accept `true` for ACK, `false` for STALL.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Response written.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 *
 * @post DCPCTR PID set accordingly; CCPL pulsed on accept.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_control_response(ra8_usb_speed_t speed, bool accept);

/**
 * @brief Push the EP0 / DCP IN data-stage payload for a control transfer.
 *
 * @details
 * The host has issued a control-IN SETUP (e.g. GET_DESCRIPTOR) and the
 * device-side stack now needs to deliver the response bytes. This call
 * selects the DCP via `CFIFOSEL.CURPIPE = 0`, then iteratively writes
 * the payload into `CFIFO` in `DCPMAXP`-sized chunks, asserting `BVAL`
 * after each chunk and waiting for `FRDY` to re-assert (which the
 * controller raises after sending the previous chunk to the host).
 * `DCPCTR.PID` is raised from NAK to BUF after the first chunk so the
 * controller answers the IN tokens. CCPL is **not** pulsed here: the
 * status stage is host-initiated and is acknowledged later when CTSQ
 * transitions to the status-stage value (handled by the bridge's CTRT
 * path).
 *
 * For `len > DCPMAXP` (the common case for CONFIGURATION descriptors),
 * the function blocks until every chunk has been handed to the
 * controller. If `len` is an exact non-zero multiple of `DCPMAXP`, no
 * terminating zero-length packet is sent here; the host asks for the
 * exact wTotalLength and is satisfied by the last full-size packet.
 *
 * @param[in] speed Which controller (FS or HS).
 * @param[in] data Pointer to the IN data-stage payload. May be
 *                 `nullptr` only if `len == 0`.
 * @param[in] len Byte count. May exceed `DCPMAXP`; the call will chunk.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Bytes written, PID raised to BUF.
 * @retval k_ra8_err_invalid_arg Bad speed / data / length combination.
 * @retval k_ra8_err_hw_timeout `FRDY` never asserted.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 * @pre A control-IN SETUP was just observed (CTSQ = read data stage).
 *
 * @post EP0 IN buffer holds the data, `DCPCTR.PID = BUF`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_dcp_in_data(ra8_usb_speed_t speed, const uint8_t* data, uint16_t len);

/**
 * @brief Arm the DCP (EP0) to receive a host-to-device control data stage.
 *
 * @details The non-blocking front half of the control-OUT receive (the OUT
 * counterpart of ::ra8_usb_dcp_in_data). Clears any stale DCP BRDY latch,
 * enables the DCP BRDY interrupt, and sets `DCPCTR.PID = BUF` so the SIE ACKs
 * the host's OUT token. Returns immediately -- the host's OUT packet then
 * raises a fresh USB IRQ where ::ra8_usb_dcp_out_read drains the bank. The
 * arm/read split is mandatory on the USB self-loop, where the FS device ISR
 * and the HS host worker thread share one CPU: a blocking receive in the
 * SETUP ISR would spin out the very thread that must send the data.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              DCP armed (BRDY enabled, PID = BUF).
 * @retval k_ra8_err_invalid_arg @p speed selects no controller.
 * @retval k_ra8_err_hw_timeout  BRDYENB read-back did not latch the DCP bit.
 *
 * @pre `ra8_usb_device_init` ran for this `speed`.
 * @pre A control-OUT SETUP with `wLength > 0` was just observed and VALID cleared.
 *
 * @post DCP BRDY is enabled and `DCPCTR.PID = BUF`.
 * @post The host's OUT data is ACKed into the DCP bank on arrival.
 *
 * @note Non-blocking; ISR-safe. Not thread-safe against a concurrent EP0 user.
 * @see ra8_usb_dcp_out_read
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_dcp_out_arm(ra8_usb_speed_t speed);

/**
 * @brief Drain an armed control-OUT data stage from the DCP (EP0).
 *
 * @details The back half of the control-OUT receive: call from the BRDY ISR
 * once ::ra8_usb_dcp_out_arm armed the DCP and the host's OUT packet landed
 * (DCP BRDY asserted). Disables the one-shot DCP BRDY, drains one buffer bank
 * via the CFIFO into @p buf, and parks `DCPCTR.PID = NAK`. Used for class
 * requests that carry a host->device data stage (e.g. DFU_DNLOAD); the
 * control-write status stage is driven separately via the CCPL pulse.
 *
 * @param[in]  speed  Which controller (FS or HS).
 * @param[out] buf    Destination for the received bytes (non-NULL).
 * @param[in]  cap    Capacity of @p buf in bytes.
 * @param[out] out_rx Receives the host's packet length in bytes (non-NULL).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              A packet (possibly zero-length) was drained.
 * @retval k_ra8_err_invalid_arg Bad speed, or @p buf / @p out_rx NULL.
 * @retval k_ra8_err_no_data     DCP BRDY is not set; the OUT packet has not landed.
 * @retval k_ra8_err_hw_timeout  CFIFO never reported FRDY for the DCP bank.
 *
 * @pre ::ra8_usb_dcp_out_arm armed the DCP for this transfer.
 * @pre Caller observed the DCP BRDY interrupt (or polls it via the no-data return).
 *
 * @post @p out_rx holds the host's DTLN; @p buf holds `min(DTLN, cap)` bytes.
 * @post DCP BRDY is disabled + cleared and `DCPCTR.PID = NAK`.
 *
 * @note Non-blocking past a bounded CFIFO FRDY wait; ISR-safe.
 * @see ra8_usb_dcp_out_arm
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_dcp_out_read(ra8_usb_speed_t speed, uint8_t* buf, uint16_t cap, uint16_t* out_rx);

/* =============================================================================
 * IRQ delivery
 * =============================================================================
 */

/**
 * @brief Install (or detach) the per-controller USB event handler.
 *
 * @details
 * Each controller (USBFS, USBHS) has its own callback slot, so one
 * firmware image can run different upper-layer drivers on each
 * controller simultaneously (e.g. USBX/DCD bridge on USBHS, a bare-CDC
 * handler on USBFS). The matching `ra8_usb_dispatch(speed)` only fires
 * the callback registered for that speed.
 *
 * @param[in] speed Which controller's callback slot to update.
 * @param[in] fn Callback. NULL detaches.
 * @param[in] ctx Context passed to `fn`.
 *
 * Slot replacement performs no fallible work; callers observe completion
 * when this function returns.
 *
 * @pre @p speed is `k_ra8_usb_speed_fs` or `k_ra8_usb_speed_hs`.
 * @pre Caller serializes this update with `ra8_usb_dispatch(speed)`.
 *
 * @post Subsequent `ra8_usb_dispatch(speed)` calls route through @p fn.
 * @post Passing NULL for @p fn prevents subsequent callback invocation for
 *       the selected controller.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
void ra8_usb_attach_handler(ra8_usb_speed_t speed, ra8_usb_event_fn_t fn, void* ctx);

/**
 * @brief Snapshot `INTSTS0` and fire the installed event handler.
 *
 * @details
 * Reads `INTSTS0`, clears it, and invokes the registered callback
 * with the snapshot. Designed to be called from the controller's
 * NVIC ISR (USBFS_INT or USBHS_INT).
 *
 * @param[in] speed Which controller fired.
 *
 * @pre None.
 *
 * @post `INTSTS0` zeroed.
 *
 * @note Re-entrant only across instances; not within a single
 * controller.
 * @since 0.1.0
 *
 * @pre Module state is consistent.
 * @post Caller-visible state matches the documented contract.
 */
void ra8_usb_dispatch(ra8_usb_speed_t speed);

/**
 * @brief Read INTSTS0 without acking any bits.
 * @details Snapshot accessor used by callers that want to inspect
 *          DVSQ / CTSQ status fields between dispatch ticks without
 *          disturbing the IRQ-bit state. Returns 0 if `speed` is invalid.
 * @param[in] speed Controller selector.
 * @return Raw INTSTS0 value.
 * @retval 0 invalid speed or controller not powered.
 * @pre Module is initialized via ra8_usb_device_init.
 * @pre Caller has the controller powered.
 * @post No INTSTS0 bits are modified.
 * @post Returned value reflects the controller at call time.
 * @note Pure MMIO read; safe to call from any context.
 * @since 0.1.0
 */
uint16_t ra8_usb_intsts0_snapshot(ra8_usb_speed_t speed);

/* =============================================================================
 * Power
 * =============================================================================
 */

/**
 * @brief Drop the controller's MSTP gate without touching SYSCFG.
 *
 * @param[in] speed Which controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok MSTP gate set.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post Controller is power-gated; register access is undefined.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_enter_stop(ra8_usb_speed_t speed);

/**
 * @brief Re-enable the controller's MSTP gate (counterpart to
 * `ra8_usb_enter_stop`).
 *
 * @param[in] speed Which controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok MSTP gate cleared.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post Controller registers are accessible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_exit_stop(ra8_usb_speed_t speed);

#ifdef __cplusplus
}
#endif
