/**
 * @file ra_usb.h
 * @brief Native USB controller driver public API (device + host modes)
 *
 * @details
 * Hand-written, FSP-equivalent driver for the two USB controllers on
 * the Renesas RA8D2 (USBFS @ 0x40250000, USBHS @ 0x40351000). Mirrors
 * the bring-up flow of FSP's `r_usb_basic` /
 * `r_usb_pdriver.c` (device) and `r_usb_hreg_access.c` (host) but
 * compiled as part of this tree, with no Renesas FSP, CherryUSB, or
 * TinyUSB binaries pulled in. The class-layer splits live in
 * `ra_usb_cdc.{h,c}` (device-side CDC ACM) and `ra_usb_hcdc.{h,c}`
 * (host-side CDC ACM).
 *
 * ## Surface
 *
 * - **Device-mode lifecycle** -- `ra_usb_device_init` / `_deinit`,
 *   plus power helpers `ra_usb_enter_stop` / `_exit_stop`.
 * - **Host-mode lifecycle** -- `ra_usb_host_init` / `_deinit`,
 *   `ra_usb_host_bus_reset`, `ra_usb_host_set_uact`,
 *   `ra_usb_host_setup_request`.
 * - **Bus** -- `ra_usb_device_attach` (raises D+ pull-up).
 * - **State** -- `ra_usb_get_device_state`, `ra_usb_set_address`.
 * - **Endpoints** -- `ra_usb_configure_endpoint`, `ra_usb_queue_in`,
 *   `ra_usb_queue_out`, `ra_usb_stall_endpoint`.
 * - **Control transfers** -- `ra_usb_read_setup_if_valid` /
 *   `ra_usb_read_setup_unconditional`,
 *   `ra_usb_control_response`.
 * - **IRQ delivery** -- `ra_usb_attach_handler` + `ra_usb_dispatch`.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_err.h"

/* =============================================================================
 * Public types
 * =============================================================================
 */

/**
 * @enum ra_usb_speed_t
 * @brief Selects which controller instance the call targets.
 */
typedef enum : uint8_t {
  k_ra_usb_speed_fs = 0U, /**< Full-Speed controller (USBFS @ 0x40250000). */
  k_ra_usb_speed_hs = 1U, /**< High-Speed controller (USBHS @ 0x40351000). */
} ra_usb_speed_t;

/**
 * @enum ra_usb_dev_state_t
 * @brief Decoded `INTSTS0.DVSQ[2:0]` device-state values.
 *
 * @details Stable enum used by the public surface. Internal masks
 * still live in `ra8d2_usb_regs.h::ra_usb_dvsq_t`.
 */
typedef enum : uint8_t {
  k_ra_usb_dev_state_powered    = 0U, /**< Powered, no reset yet. */
  k_ra_usb_dev_state_default    = 1U, /**< Default (post reset).  */
  k_ra_usb_dev_state_address    = 2U, /**< Address assigned.      */
  k_ra_usb_dev_state_configured = 3U, /**< Configured.            */
  k_ra_usb_dev_state_suspended  = 4U, /**< Suspended.             */
} ra_usb_dev_state_t;

/**
 * @enum ra_usb_ep_dir_t
 * @brief Endpoint direction.
 */
typedef enum : uint8_t {
  k_ra_usb_ep_dir_out = 0U, /**< Host -> device. */
  k_ra_usb_ep_dir_in  = 1U, /**< Device -> host. */
} ra_usb_ep_dir_t;

/**
 * @enum ra_usb_ep_type_t
 * @brief Transfer type for a non-control endpoint.
 */
typedef enum : uint8_t {
  k_ra_usb_ep_type_bulk = 0U, /**< Bulk transfer.       */
  k_ra_usb_ep_type_intr = 1U, /**< Interrupt transfer.  */
  k_ra_usb_ep_type_iso  = 2U, /**< Isochronous transfer.*/
} ra_usb_ep_type_t;

/**
 * @struct ra_usb_setup_t
 * @brief Decoded 8-byte USB SETUP packet.
 *
 * @details Mirrors USB 2.0 chapter 9 layout. Populated by
 * `ra_usb_read_setup_if_valid` / `ra_usb_read_setup_unconditional`
 * from the controller's USBREQ / USBVAL /
 * USBINDX / USBLENG mirror registers.
 */
typedef struct {
  uint8_t  bm_request_type; /**< Request direction / type / recipient. */
  uint8_t  b_request;       /**< bRequest code.                        */
  uint16_t w_value;         /**< wValue.                               */
  uint16_t w_index;         /**< wIndex.                               */
  uint16_t w_length;        /**< wLength.                              */
} ra_usb_setup_t;

/**
 * @typedef ra_usb_event_fn_t
 * @brief USB event callback signature.
 *
 * @param[in] ctx Caller-supplied context.
 * @param[in] speed Which controller fired.
 * @param[in] status_mask Snapshot of `INTSTS0` at dispatch time.
 *
 * @note Invoked from the dispatch site (typically ISR context).
 */
typedef void (*ra_usb_event_fn_t)(void* ctx, ra_usb_speed_t speed, uint16_t status_mask);

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
 * `ra_usb_device_attach` once descriptors are wired up.
 *
 * @param[in] speed Which controller to bring up.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Controller ready, D+ pull-up off.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_hw_init_failed MSTP release failed.
 *
 * @pre Caller holds single-threaded init context (or IRQs masked).
 * @pre `ra_mstp_init` and `ra_pwr_init` have run.
 *
 * @post Controller is clocked, D+ pull-up off, IRQs unmasked at
 * controller level (NVIC line still owned by `ra_irq`).
 * @post `ra_usb_get_device_state` returns
 * `k_ra_usb_dev_state_powered`.
 *
 * @note Not thread-safe.
 * @see ra_usb_device_attach
 * @see ra_usb_device_deinit
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_init(ra_usb_speed_t speed);

/**
 * @brief Tear down a USB controller and drop its MSTP reference.
 *
 * @param[in] speed Which controller to release.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Controller released.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `SYSCFG`, `INTENB0`, `INTENB1` all zero.
 * @post Controller MSTP-gated.
 *
 * @note Not thread-safe.
 * @see ra_usb_device_init
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_deinit(ra_usb_speed_t speed);

/**
 * @brief Raise / drop the D+ pull-up to advertise the device to the host.
 *
 * @param[in] speed Which controller.
 * @param[in] attached `true` to assert pull-up, `false` to drop.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Pull-up state updated.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre `ra_usb_device_init` has been called for this `speed`.
 *
 * @post On success, `SYSCFG.DPRPU` matches `attached`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_attach(ra_usb_speed_t speed, bool attached);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Mask returned.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_null_ptr `out_mask` was NULL.
 *
 * @pre `out_mask` non-NULL.
 *
 * @post No controller state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_get_status(ra_usb_speed_t speed, uint16_t* out_mask);

/**
 * @brief Clear bits in `INTSTS0`.
 *
 * @param[in] speed Which controller.
 * @param[in] mask Bits to clear.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Bits cleared.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post `INTSTS0 & mask == 0`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_clear_status(ra_usb_speed_t speed, uint16_t mask);

/**
 * @brief Read the decoded device state from `INTSTS0.DVSQ[2:0]`.
 *
 * @param[in] speed Which controller.
 * @param[out] out_state Receives the decoded state.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok State read.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_null_ptr `out_state` was NULL.
 *
 * @pre `out_state` non-NULL.
 *
 * @post No controller state mutated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_get_device_state(ra_usb_speed_t speed, ra_usb_dev_state_t* out_state);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Address written.
 * @retval k_ra_err_invalid_arg `speed` out of range or address > 127.
 *
 * @pre Bus is in default state (post reset, pre SET_ADDRESS).
 *
 * @post `USBADDR.USBADDR[6:0] == address`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_set_address(ra_usb_speed_t speed, uint8_t address);

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
 * @param[in] speed Which controller (`k_ra_usb_speed_fs` or `_hs`).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok DCP re-armed.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre `ra_usb_device_init` ran for this `speed`.
 * @pre Caller observed a `DVST` interrupt with `DVSQ == default`.
 *
 * @post `DCPMAXP == 64`, `DCPCTR == 0`, all `PIPECTR[i]` PID == NAK.
 * @post `INTENB0` matches the post-init device-mode mask.
 *
 * @note Safe to call from IRQ-callback context.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_device_busreset_rearm(ra_usb_speed_t speed);

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
 * stack can queue data with `ra_usb_queue_in` /
 * `ra_usb_queue_out` before transitioning to BUF.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 1..9.
 * @param[in] ep_addr Endpoint address (USB EP number, 1..15).
 * @param[in] dir Endpoint direction.
 * @param[in] type Endpoint type.
 * @param[in] max_packet Maximum packet size.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Pipe configured.
 * @retval k_ra_err_invalid_arg Pipe / EP / dir / type / size invalid.
 *
 * @pre `ra_usb_device_init` ran for this `speed`.
 *
 * @post Pipe responds NAK, software toggle cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_configure_endpoint(ra_usb_speed_t   speed,
                                                 uint8_t          pipe_num,
                                                 uint8_t          ep_addr,
                                                 ra_usb_ep_dir_t  dir,
                                                 ra_usb_ep_type_t type,
                                                 uint16_t         max_packet);

/**
 * @brief Stall a configured endpoint.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 0..9 (0 = DCP).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok PID set to STALL.
 * @retval k_ra_err_invalid_arg Pipe out of range.
 *
 * @pre `ra_usb_device_init` ran for this `speed`.
 *
 * @post Pipe responds STALL until the stack clears the halt.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_stall_endpoint(ra_usb_speed_t speed, uint8_t pipe_num);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Transfer queued.
 * @retval k_ra_err_invalid_arg Pipe / len / data combination invalid.
 * @retval k_ra_err_hw_timeout `FRDY` never asserted.
 *
 * @pre Pipe previously configured via
 * `ra_usb_configure_endpoint(..., dir = IN, ...)`.
 *
 * @post Pipe PID set to BUF.
 * @post `BVAL` asserted on the FIFO port.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_usb_queue_in(ra_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len);

/**
 * @brief Drain an OUT transfer (host -> device) from `pipe_num`.
 *
 * @details
 * Counterpart to `ra_usb_queue_in`. Selects `pipe_num`, waits for
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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Bytes read.
 * @retval k_ra_err_no_data Buffer was empty.
 * @retval k_ra_err_invalid_arg Pipe / pointers invalid.
 * @retval k_ra_err_hw_timeout `FRDY` never asserted.
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
[[nodiscard]] ra_err_t ra_usb_queue_out(ra_usb_speed_t speed,
                                        uint8_t        pipe_num,
                                        uint8_t*       out_buf,
                                        uint16_t*      inout_len,
                                        bool           rearm);

/**
 * @brief Re-arm an OUT pipe that the controller has parked at PID=NAK.
 *
 * @details
 * The RA8D2 USB-FS / USB-HS pipe state machine auto-flips PID from BUF
 * to NAK after every successful BUF cycle on a single-buffered pipe
 * (HUM Ch 36 -- single-buffered OUT pipes). `ra_usb_queue_out` leaves
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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok               Pipe re-armed at PID=BUF, NRDYSTS bit acked.
 * @retval k_ra_err_invalid_arg  `speed` invalid or `pipe_num` out of range.
 *
 * @pre Pipe was previously configured via `ra_usb_configure_endpoint`
 *      with `dir = k_ra_usb_ep_dir_out`.
 * @pre Caller serialises against `ra_usb_queue_out` for the same pipe.
 *
 * @post `NRDYSTS` bit `pipe_num` is cleared.
 * @post `PIPECTR[pipe_num-1].PID == BUF`.
 *
 * @note Not thread-safe; the polled-dispatch worker is the sole caller
 *       on hardware.
 *
 * @see ra_usb_queue_out
 * @see ra_usb_configure_endpoint
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_rearm_out_pipe(ra_usb_speed_t speed, uint8_t pipe_num);

/**
 * @brief Park an OUT pipe at PID=NAK so an idle pipe stays quiescent.
 *
 * @details
 * The inverse of ::ra_usb_rearm_out_pipe. An OUT pipe left at PID=BUF
 * with its per-pipe BRDY enabled but with no consumer ready will ACK
 * the next host OUT token into the pipe FIFO, latch BRDYSTS, and --
 * because nothing drains it -- hold INTSTS0.BRDY asserted, which
 * re-fires the USB ISR continuously and starves RTOS thread mode.
 * Parking the pipe at PID=NAK makes the controller NAK host OUT
 * tokens (normal USB flow control: the host simply retries), so no
 * data enters the FIFO, BRDYSTS never latches, and the ISR stays
 * quiet until a consumer arms the pipe with ::ra_usb_rearm_out_pipe.
 *
 * @param[in] speed    Which controller (FS / HS).
 * @param[in] pipe_num PIPE1..PIPE9 (must not be 0; DCP uses DCPCTR).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok               Pipe parked at PID=NAK.
 * @retval k_ra_err_invalid_arg  `speed` invalid or `pipe_num` out of range.
 *
 * @pre Pipe was previously configured via `ra_usb_configure_endpoint`
 *      with `dir = k_ra_usb_ep_dir_out`.
 * @pre No transfer is currently in flight on the pipe.
 *
 * @post `PIPECTR[pipe_num-1].PID == NAK`.
 *
 * @note Not thread-safe; caller serialises against `ra_usb_rearm_out_pipe`.
 *
 * @see ra_usb_rearm_out_pipe
 * @see ra_usb_configure_endpoint
 *
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_park_out_pipe(ra_usb_speed_t speed, uint8_t pipe_num);

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
 * `k_ra_err_no_data` when `VALID == 0` so the FS / CTRT-driven path
 * can poll the same routine without producing stale dispatches. Use
 * ::ra_usb_read_setup_unconditional on the HS / SQMON polled-worker
 * path where the SIE may auto-clear `VALID` before the worker observes
 * the SETUP edge.
 *
 * @param[in] speed Which controller.
 * @param[out] out_setup Decoded 8-byte SETUP packet.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok SETUP captured.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_null_ptr `out_setup` was NULL.
 * @retval k_ra_err_no_data `INTSTS0.VALID` was clear.
 *
 * @pre `out_setup` non-NULL.
 *
 * @post `INTSTS0.VALID` cleared on success.
 *
 * @note Call from `CTRT`-handling path. HS dispatcher must use
 *       ::ra_usb_read_setup_unconditional instead.
 *
 * @see ra_usb_read_setup_unconditional
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_read_setup_if_valid(ra_usb_speed_t speed, ra_usb_setup_t* out_setup);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok SETUP drained from captured registers.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_null_ptr `out_setup` was NULL.
 *
 * @pre `out_setup` non-NULL.
 * @pre Caller has independent proof a SETUP arrived (e.g. SQMON==1).
 *
 * @post `*out_setup` mirrors USBREQ/USBVAL/USBINDX/USBLENG.
 * @post `INTSTS0.VALID` is W0C-cleared.
 *
 * @note Call from the HS / SQMON polled-dispatcher path.
 *
 * @see ra_usb_read_setup_if_valid
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_read_setup_unconditional(ra_usb_speed_t  speed,
                                                       ra_usb_setup_t* out_setup);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Response written.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre `ra_usb_device_init` ran for this `speed`.
 *
 * @post DCPCTR PID set accordingly; CCPL pulsed on accept.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_control_response(ra_usb_speed_t speed, bool accept);

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
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Bytes written, PID raised to BUF.
 * @retval k_ra_err_invalid_arg Bad speed / data / length combination.
 * @retval k_ra_err_hw_timeout `FRDY` never asserted.
 *
 * @pre `ra_usb_device_init` ran for this `speed`.
 * @pre A control-IN SETUP was just observed (CTSQ = read data stage).
 *
 * @post EP0 IN buffer holds the data, `DCPCTR.PID = BUF`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_dcp_in_data(ra_usb_speed_t speed, const uint8_t* data, uint16_t len);

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
 * handler on USBFS). The matching `ra_usb_dispatch(speed)` only fires
 * the callback registered for that speed.
 *
 * @param[in] speed Which controller's callback slot to update.
 * @param[in] fn Callback. NULL detaches.
 * @param[in] ctx Context passed to `fn`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Handler installed.
 *
 * @pre None.
 *
 * @post Subsequent `ra_usb_dispatch(speed)` calls route through `fn`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_attach_handler(ra_usb_speed_t speed, ra_usb_event_fn_t fn, void* ctx);

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
void ra_usb_dispatch(ra_usb_speed_t speed);

/**
 * @brief Read INTSTS0 without acking any bits.
 * @details Snapshot accessor used by callers that want to inspect
 *          DVSQ / CTSQ status fields between dispatch ticks without
 *          disturbing the IRQ-bit state. Returns 0 if `speed` is invalid.
 * @param[in] speed Controller selector.
 * @return Raw INTSTS0 value.
 * @retval 0 invalid speed or controller not powered.
 * @pre Module is initialized via ra_usb_device_init.
 * @pre Caller has the controller powered.
 * @post No INTSTS0 bits are modified.
 * @post Returned value reflects the controller at call time.
 * @note Pure MMIO read; safe to call from any context.
 * @since 0.1.0
 */
uint16_t ra_usb_intsts0_snapshot(ra_usb_speed_t speed);

/* =============================================================================
 * Power
 * =============================================================================
 */

/**
 * @brief Drop the controller's MSTP gate without touching SYSCFG.
 *
 * @param[in] speed Which controller.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok MSTP gate set.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post Controller is power-gated; register access is undefined.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_enter_stop(ra_usb_speed_t speed);

/**
 * @brief Re-enable the controller's MSTP gate (counterpart to
 * `ra_usb_enter_stop`).
 *
 * @param[in] speed Which controller.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok MSTP gate cleared.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre None.
 *
 * @post Controller registers are accessible.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_exit_stop(ra_usb_speed_t speed);

/* =============================================================================
 * Host-mode bring-up (peer of the device-mode lifecycle above)
 * =============================================================================
 */

/**
 * @brief Bring up a USB controller in HOST mode.
 *
 * @details
 * Mirrors FSP's `hw_usb_hmodule_init`
 * (`r_usb_basic/src/hw/r_usb_hreg_access.c`). Bring-up sequence:
 *
 *  1. Drop the controller's MSTP gate.
 *  2. Drive `SYSCFG.SCKE` and wait for the clock to be stable.
 *  3. Set `SYSCFG.DCFM = 1` (host) and `SYSCFG.DRPD = 1` (D+/D-
 *     pull-down so the bus floats low when no device is attached).
 *  4. Clear `SYSCFG.DPRPU` (device pull-up has no meaning in host mode).
 *  5. Set `SYSCFG.HSE` for the HS instance only.
 *  6. Set `SYSCFG.USBE`.
 *  7. Configure the DCP (default control pipe) for host control
 *     transfers with a 64-byte max packet.
 *
 * Bus is left idle (UACT = 0). Use `ra_usb_host_set_uact(true)` to
 * start SOF generation once a device has been detected on
 * `SYSSTS0.LNST`.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Controller in host mode, bus floating.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_hw_init_failed MSTP enable failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra_mstp_init` and `ra_pwr_init` already ran.
 *
 * @post `SYSCFG.DCFM = 1`, `SYSCFG.DRPD = 1`, `SYSCFG.USBE = 1`.
 * @post `DVSTCTR0.UACT = 0` (bus idle, no SOF).
 *
 * @note Not thread-safe.
 * @see ra_usb_host_set_uact
 * @see ra_usb_host_bus_reset
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_init(ra_usb_speed_t speed);

/**
 * @brief Tear down a host-mode controller and drop its MSTP reference.
 *
 * @param[in] speed Which controller.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Controller released.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `SYSCFG = 0`, `DVSTCTR0 = 0`, MSTP gated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_deinit(ra_usb_speed_t speed);

/**
 * @brief Drive (or release) the bus reset line.
 *
 * @details
 * Sets / clears `DVSTCTR0.USBRST`. While reset is asserted, `UACT`
 * is forced low (no SOF). The caller is expected to hold reset for
 * at least the USB-spec-required 10 ms before deasserting and then
 * re-enabling SOF generation via `ra_usb_host_set_uact(true)`.
 *
 * Mirrors FSP's `usb_hstd_bus_reset` SET phase
 * (`r_usb_hreg_abs.c`).
 *
 * @param[in] speed Which controller.
 * @param[in] assert_reset `true` to drive USBRST, `false` to release.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Reset line updated.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre `ra_usb_host_init` ran for this `speed`.
 *
 * @post `DVSTCTR0.USBRST` matches `assert_reset`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_bus_reset(ra_usb_speed_t speed, bool assert_reset);

/**
 * @brief Enable / disable SOF (Start-of-Frame) generation on the bus.
 *
 * @details
 * Toggles `DVSTCTR0.UACT`. With UACT = 1 the controller starts
 * generating SOF tokens at the spec-required cadence (1 ms FS / 125
 * us HS). UACT = 0 leaves the bus idle.
 *
 * @param[in] speed Which controller.
 * @param[in] enable `true` to enable SOF generation, `false` to halt.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok UACT updated.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 *
 * @pre `ra_usb_host_init` ran for this `speed`.
 *
 * @post `DVSTCTR0.UACT` matches `enable`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_set_uact(ra_usb_speed_t speed, bool enable);

/**
 * @brief Issue a SETUP packet through the DCP (host -> peripheral).
 *
 * @details
 * Programs `USBREQ`, `USBVAL`, `USBINDX`, `USBLENG` from `setup` and
 * sets `DCPCTR.SUREQ` so the controller transmits an 8-byte SETUP
 * stage on the next bus frame. The data and status stages are
 * driven by `ra_usb_queue_in` / `ra_usb_queue_out` on the DCP, plus
 * subsequent `DCPCTR.PID` / `DCPCTR.CCPL` writes via
 * `ra_usb_control_response`. This wraps FSP's
 * `usb_hstd_setup_command` low-level path.
 *
 * @param[in] speed Which controller.
 * @param[in] setup The 8-byte SETUP packet to transmit.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok SETUP queued; controller will transmit on next frame.
 * @retval k_ra_err_invalid_arg `speed` out of range.
 * @retval k_ra_err_null_ptr `setup` was NULL.
 * @retval k_ra_err_busy `DCPCTR.SUREQ` was still asserted from a
 *                       prior request.
 *
 * @pre `ra_usb_host_init` ran for this `speed`.
 * @pre `setup` non-NULL.
 *
 * @post `USBREQ`, `USBVAL`, `USBINDX`, `USBLENG` all loaded.
 * @post `DCPCTR.SUREQ = 1` until the controller clears it.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_setup_request(ra_usb_speed_t speed, const ra_usb_setup_t* setup);

/**
 * @brief Run a complete host control transfer (SETUP, optional DATA, STATUS).
 *
 * @details
 * Polled, synchronous control transfer over the DCP (pipe 0). Drives the
 * SETUP stage (SUREQ + wait for self-clear), an optional DATA-IN stage for
 * device-to-host requests (BRDY-gated CFIFO reads), and the zero-length
 * STATUS stage (CCPL). Completion is detected via SUREQ-clear / BRDY / BEMP
 * -- the host-mode signals -- not CTRT, which only fires in device mode.
 * This is the host counterpart of the device-mode control path and the
 * primitive the host class drivers (MSC/HID/CDC) build enumeration on.
 *
 * @param[in]  speed        Which controller (FS or HS).
 * @param[in]  setup        The 8-byte SETUP packet.
 * @param[out] data         Buffer for the DATA-IN stage (may be NULL for
 *                          no-data / control-write requests).
 * @param[in]  data_len     Capacity of @p data in bytes.
 * @param[out] out_received Optional; receives DATA-IN bytes read (0 if none).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok               Transfer completed through the status stage.
 * @retval k_ra_err_invalid_arg  `speed` out of range.
 * @retval k_ra_err_null_ptr     `setup` was NULL.
 * @retval k_ra_err_busy         A control transfer was already pending.
 * @retval k_ra_err_hw_timeout   A stage did not complete within the bound.
 *
 * @pre `ra_usb_host_init` ran; the bus is reset and `DVSTCTR0.UACT = 1`.
 * @pre `setup` is non-NULL; `data` holds at least `data_len` bytes if used.
 *
 * @post On k_ra_ok the device has seen the request and any IN data is in
 *       @p data with the count in @p out_received.
 * @post The DCP PID is left NAK.
 *
 * @note Blocking; each stage is bounded by an internal spin limit.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_control_xfer(ra_usb_speed_t        speed,
                                                const ra_usb_setup_t* setup,
                                                uint8_t*              data,
                                                uint16_t              data_len,
                                                uint16_t*             out_received);

/**
 * @brief Last stage reached by ::ra_usb_host_control_xfer (bring-up diag).
 *
 * @details 0=before SETUP, 1=SETUP done, 2=DATA-IN entered, 3=first packet
 * received, 4=DATA done, 5=STATUS entered, 6=STATUS done. Lets a caller
 * pinpoint where a timed-out control transfer stalled.
 *
 * @return The stage code from the most recent control transfer.
 * @retval 0 No control transfer has run since reset.
 * @retval 6 The most recent control transfer closed cleanly.
 * @pre ::ra_usb_host_init ran for the controller of interest.
 * @pre At least one ::ra_usb_host_control_xfer may have been attempted.
 * @post No state is modified.
 * @post The returned value reflects the last transfer only.
 * @note Diagnostic only; not part of the transfer contract.
 * @since 0.1.0
 */
uint8_t ra_usb_host_ctrl_stage(void);

/**
 * @brief Retarget the host's default control pipe at a device address.
 *
 * @details
 * Programs the DEVADDn slot for @p dev_addr with the connected link speed
 * (copied from `DVSTCTR0.RHST`) and loads `DCPMAXP.DEVSEL` so subsequent
 * control transfers carry tokens addressed to @p dev_addr. Call after a
 * successful SET_ADDRESS control transfer (plus the 2 ms set-address
 * recovery the USB spec grants the device).
 *
 * @param[in] speed    Which controller (FS or HS).
 * @param[in] dev_addr Address the device was assigned (0..10).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok              The DCP now targets @p dev_addr.
 * @retval k_ra_err_invalid_arg `speed` or `dev_addr` out of range.
 *
 * @pre `ra_usb_host_init` ran; the bus reset completed (RHST valid).
 * @pre No control transfer is in flight on the DCP.
 *
 * @post `DCPMAXP.DEVSEL` equals @p dev_addr with MXPS preserved.
 * @post The DEVADDn slot for @p dev_addr carries the link speed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_set_target(ra_usb_speed_t speed, uint8_t dev_addr);

/**
 * @brief Configure a controller pipe against an attached device's bulk endpoint.
 *
 * @details
 * Host-mode counterpart of `ra_usb_configure_endpoint`: programs
 * PIPECFG (bulk type; DIR mapped so device-to-host = receiving),
 * PIPEBUF, and PIPEMAXP including `DEVSEL` = @p dev_addr, then forces the
 * DATA0 sequence (SQCLR) and parks the pipe NAK with its BRDY/NRDY/BEMP
 * status bits cleared. Use PIPE1 for the device's bulk-IN endpoint and
 * PIPE2 for its bulk-OUT endpoint in the MSC bring-up.
 *
 * @param[in] speed          Which controller (FS or HS).
 * @param[in] pipe_num       Controller pipe to program (1..9).
 * @param[in] dev_addr       Target device address (0..10).
 * @param[in] ep_num         Device endpoint number (1..15, no direction bit).
 * @param[in] device_to_host `true` for the device's IN endpoint (host
 *                           receives), `false` for its OUT endpoint.
 * @param[in] max_packet     Endpoint wMaxPacketSize from the descriptor.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok              Pipe configured, DATA0 forced, parked NAK.
 * @retval k_ra_err_invalid_arg An argument is out of range.
 *
 * @pre `ra_usb_host_init` ran and SET_CONFIGURATION reset the endpoint.
 * @pre The pipe is not currently armed (no transfer in flight).
 *
 * @post The pipe targets @p dev_addr endpoint @p ep_num with PID = NAK.
 * @post The pipe's BRDY/NRDY/BEMP status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_pipe_setup(ra_usb_speed_t speed,
                                              uint8_t        pipe_num,
                                              uint8_t        dev_addr,
                                              uint8_t        ep_num,
                                              bool           device_to_host,
                                              uint16_t       max_packet);

/**
 * @brief Transmit one bulk packet to the device and wait for the ACK.
 *
 * @details
 * Single-packet host bulk OUT: stages @p data into the pipe buffer
 * (FRDY-gated FIFO write + BVAL + PID=BUF via `ra_usb_queue_in`), then
 * waits for the buffer-empty (BEMP) event that marks the packet on the
 * wire and acknowledged, and parks the pipe NAK. Sized for protocol
 * headers such as the 31-byte MSC CBW; @p len must not exceed the pipe's
 * max packet size.
 *
 * @param[in] speed    Which controller (FS or HS).
 * @param[in] pipe_num Pipe configured by `ra_usb_host_pipe_setup` (OUT).
 * @param[in] data     Packet payload.
 * @param[in] len      Payload length in bytes (<= pipe MPS).
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok              Packet transmitted and ACKed.
 * @retval k_ra_err_invalid_arg `speed` / `pipe_num` / `len` out of range.
 * @retval k_ra_err_null_ptr    `data` was NULL.
 * @retval k_ra_err_hw_error    The device STALLed the endpoint.
 * @retval k_ra_err_hw_timeout  No ACK before the spin bound.
 *
 * @pre The pipe was configured by `ra_usb_host_pipe_setup`.
 * @pre The device is configured (SET_CONFIGURATION accepted).
 *
 * @post The pipe PID is NAK and its BEMP status is cleared.
 * @post On k_ra_ok the device has ACKed the packet.
 *
 * @note Blocking; bounded by an internal spin limit.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_usb_host_bulk_out(ra_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len);

/**
 * @brief Receive a bulk transfer from the device into @p buf.
 *
 * @details
 * Arms the IN pipe (PID=BUF, so the SIE issues IN tokens) and consumes
 * BRDY-gated packets from the CFIFO until the device ends the transfer
 * with a short packet or @p max_len bytes have arrived, then parks the
 * pipe NAK. Used for the MSC data stage and the 13-byte CSW.
 *
 * @param[in]  speed        Which controller (FS or HS).
 * @param[in]  pipe_num     Pipe configured by `ra_usb_host_pipe_setup` (IN).
 * @param[out] buf          Destination buffer.
 * @param[in]  max_len      Capacity of @p buf in bytes.
 * @param[out] out_received Receives the byte count gathered.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok                Transfer ended (short packet or count).
 * @retval k_ra_err_invalid_arg   `speed` / `pipe_num` out of range.
 * @retval k_ra_err_null_ptr      `buf` or `out_received` was NULL.
 * @retval k_ra_err_invalid_state The pipe has no max-packet programmed.
 * @retval k_ra_err_hw_error      The device STALLed the endpoint.
 * @retval k_ra_err_hw_timeout    No packet before the spin bound.
 *
 * @pre The pipe was configured by `ra_usb_host_pipe_setup`.
 * @pre The device is configured and expecting to send (e.g. post-CBW).
 *
 * @post The pipe PID is NAK; `*out_received` holds the byte count.
 * @post On error the partial count gathered so far is still reported.
 *
 * @note Blocking; bounded by an internal spin limit per packet.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_host_bulk_in(ra_usb_speed_t speed,
                                           uint8_t        pipe_num,
                                           uint8_t*       buf,
                                           uint16_t       max_len,
                                           uint16_t*      out_received);

#ifdef __cplusplus
}
#endif
