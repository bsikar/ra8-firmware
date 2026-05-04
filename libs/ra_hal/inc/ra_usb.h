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
 * - **Control transfers** -- `ra_usb_read_setup`,
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
 * `ra_usb_read_setup` from the controller's USBREQ / USBVAL /
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
 * `FRDY`, reads up to `*inout_len` bytes from the FIFO, asserts
 * `BCLR` to release the buffer, and re-arms the pipe PID to BUF so
 * the next OUT token is acknowledged.
 *
 * @param[in] speed Which controller.
 * @param[in] pipe_num PIPE number 1..9.
 * @param[out] out_buf Receive buffer.
 * @param[in,out] inout_len On entry: capacity. On exit: bytes read.
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
 * @post Pipe PID set to BUF.
 * @post `*inout_len` reflects actual byte count delivered.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t
ra_usb_queue_out(ra_usb_speed_t speed, uint8_t pipe_num, uint8_t* out_buf, uint16_t* inout_len);

/**
 * @brief Re-arm an OUT pipe that the controller has parked at PID=NAK.
 *
 * @details
 * The RA8D2 USB-FS / USB-HS pipe state machine auto-flips PID from BUF
 * to NAK after every successful BUF cycle on a single-buffered pipe
 * (HUM Ch 36 -- single-buffered OUT pipes). The drain path in
 * `ra_usb_queue_out` re-arms PID=BUF after a successful read, but
 * between the drain and the next host OUT token there is a window in
 * which the controller may NAK an incoming transaction (NRDYSTS
 * accumulates the NAK responses). This helper clears the per-pipe
 * NRDYSTS bit (W0C) and unconditionally re-asserts PID=BUF so the
 * pipe is ready to ACK the next host OUT token. Safe to call
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

/* =============================================================================
 * Control transfers (EP0 / DCP)
 * =============================================================================
 */

/**
 * @brief Snapshot the current SETUP packet from the controller.
 *
 * @details
 * Reads the four mirror registers `USBREQ`, `USBVAL`, `USBINDX`,
 * `USBLENG` and clears `INTSTS0.VALID`. The mirrors are valid only
 * while `INTSTS0.VALID == 1`; the stack must call this from the
 * `CTRT` ISR path before re-enabling further SETUP capture.
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
 * @note Call from `CTRT`-handling ISR only.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_read_setup(ra_usb_speed_t speed, ra_usb_setup_t* out_setup);

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
 * @brief Install (or detach) the shared USB event handler.
 *
 * @param[in] fn Callback. NULL detaches.
 * @param[in] ctx Context passed to `fn`.
 *
 * @return `ra_err_t` error code.
 * @retval k_ra_ok Handler installed.
 *
 * @pre None.
 *
 * @post Subsequent `ra_usb_dispatch` calls route through `fn`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_usb_attach_handler(ra_usb_event_fn_t fn, void* ctx);

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
 * @pre Module is initialised via ra_usb_device_init.
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

#ifdef __cplusplus
}
#endif
