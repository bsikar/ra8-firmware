/**
 * @file ra8_usb_host.h
 * @brief Native USB controller driver public API -- host-mode bring-up
 * @ingroup grp_hal_usb
 *
 * @details
 * Host-mode half of the hand-written, FSP-equivalent driver for the two
 * USB controllers on the Renesas RA8D2 (USBFS @ 0x40250000,
 * USBHS @ 0x40351000). Mirrors the bring-up flow of FSP's
 * `r_usb_hreg_access.c` (host) but compiled as part of this tree, with
 * no Renesas FSP, CherryUSB, or TinyUSB binaries pulled in. This
 * sub-header carries the host-mode lifecycle, bus-control, target/pipe
 * setup, control-transfer, and bulk-transfer surface; the shared public
 * types and the device-mode surface live in `ra8_usb_device.h`. Both are
 * aggregated by the thin umbrella `ra8_usb.h`.
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
#include "ra8_usb_device.h"

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
 * Bus is left idle (UACT = 0). Use `ra8_usb_host_set_uact(true)` to
 * start SOF generation once a device has been detected on
 * `SYSSTS0.LNST`.
 *
 * @param[in] speed Which controller (FS or HS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Controller in host mode, bus floating.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_hw_init_failed MSTP enable failed.
 *
 * @pre Single-threaded init context.
 * @pre `ra8_mstp_init` and `ra8_pwr_init` already ran.
 *
 * @post `SYSCFG.DCFM = 1`, `SYSCFG.DRPD = 1`, `SYSCFG.USBE = 1`.
 * @post `DVSTCTR0.UACT = 0` (bus idle, no SOF).
 *
 * @note Not thread-safe.
 * @see ra8_usb_host_set_uact
 * @see ra8_usb_host_bus_reset
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_init(ra8_usb_speed_t speed);

/**
 * @brief Tear down a host-mode controller and drop its MSTP reference.
 *
 * @param[in] speed Which controller.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Controller released.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre Single-threaded shutdown context.
 *
 * @post `SYSCFG = 0`, `DVSTCTR0 = 0`, MSTP gated.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_deinit(ra8_usb_speed_t speed);

/**
 * @brief Drive (or release) the bus reset line.
 *
 * @details
 * Sets / clears `DVSTCTR0.USBRST`. While reset is asserted, `UACT`
 * is forced low (no SOF). The caller is expected to hold reset for
 * at least the USB-spec-required 10 ms before deasserting and then
 * re-enabling SOF generation via `ra8_usb_host_set_uact(true)`.
 *
 * Mirrors FSP's `usb_hstd_bus_reset` SET phase
 * (`r_usb_hreg_abs.c`).
 *
 * @param[in] speed Which controller.
 * @param[in] assert_reset `true` to drive USBRST, `false` to release.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok Reset line updated.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre `ra8_usb_host_init` ran for this `speed`.
 *
 * @post `DVSTCTR0.USBRST` matches `assert_reset`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_bus_reset(ra8_usb_speed_t speed, bool assert_reset);

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
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok UACT updated.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 *
 * @pre `ra8_usb_host_init` ran for this `speed`.
 *
 * @post `DVSTCTR0.UACT` matches `enable`.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_set_uact(ra8_usb_speed_t speed, bool enable);

/**
 * @brief Issue a SETUP packet through the DCP (host -> peripheral).
 *
 * @details
 * Programs `USBREQ`, `USBVAL`, `USBINDX`, `USBLENG` from `setup` and
 * sets `DCPCTR.SUREQ` so the controller transmits an 8-byte SETUP
 * stage on the next bus frame. The data and status stages are
 * driven by `ra8_usb_queue_in` / `ra8_usb_queue_out` on the DCP, plus
 * subsequent `DCPCTR.PID` / `DCPCTR.CCPL` writes via
 * `ra8_usb_control_response`. This wraps FSP's
 * `usb_hstd_setup_command` low-level path.
 *
 * @param[in] speed Which controller.
 * @param[in] setup The 8-byte SETUP packet to transmit.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok SETUP queued; controller will transmit on next frame.
 * @retval k_ra8_err_invalid_arg `speed` out of range.
 * @retval k_ra8_err_null_ptr `setup` was NULL.
 * @retval k_ra8_err_busy `DCPCTR.SUREQ` was still asserted from a
 *                       prior request.
 *
 * @pre `ra8_usb_host_init` ran for this `speed`.
 * @pre `setup` non-NULL.
 *
 * @post `USBREQ`, `USBVAL`, `USBINDX`, `USBLENG` all loaded.
 * @post `DCPCTR.SUREQ = 1` until the controller clears it.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_setup_request(ra8_usb_speed_t        speed,
                                                   const ra8_usb_setup_t* setup);

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
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok               Transfer completed through the status stage.
 * @retval k_ra8_err_invalid_arg  `speed` out of range.
 * @retval k_ra8_err_null_ptr     `setup` was NULL.
 * @retval k_ra8_err_busy         A control transfer was already pending.
 * @retval k_ra8_err_hw_timeout   A stage did not complete within the bound.
 *
 * @pre `ra8_usb_host_init` ran; the bus is reset and `DVSTCTR0.UACT = 1`.
 * @pre `setup` is non-NULL; `data` holds at least `data_len` bytes if used.
 *
 * @post On k_ra8_ok the device has seen the request and any IN data is in
 *       @p data with the count in @p out_received.
 * @post The DCP PID is left NAK.
 *
 * @note Blocking; each stage is bounded by an internal spin limit.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_control_xfer(ra8_usb_speed_t        speed,
                                                  const ra8_usb_setup_t* setup,
                                                  uint8_t*               data,
                                                  uint16_t               data_len,
                                                  uint16_t*              out_received);

/**
 * @brief Last stage reached by ::ra8_usb_host_control_xfer (bring-up diag).
 *
 * @details 0=before SETUP, 1=SETUP done, 2=DATA-IN entered, 3=first packet
 * received, 4=DATA done, 5=STATUS entered, 6=STATUS done. Lets a caller
 * pinpoint where a timed-out control transfer stalled.
 *
 * @return The stage code from the most recent control transfer.
 * @retval 0 No control transfer has run since reset.
 * @retval 6 The most recent control transfer closed cleanly.
 * @pre ::ra8_usb_host_init ran for the controller of interest.
 * @pre At least one ::ra8_usb_host_control_xfer may have been attempted.
 * @post No state is modified.
 * @post The returned value reflects the last transfer only.
 * @note Diagnostic only; not part of the transfer contract.
 * @since 0.1.0
 */
uint8_t ra8_usb_host_ctrl_stage(void);

/**
 * @brief Read the host port's D+/D- line state (SYSSTS0.LNST).
 *
 * @details 0 = SE0 (no device pull-up / port idle), 1 = J-state (a
 * full-speed-signalling device is attached), 2 = K-state. Used by host
 * class drivers to wait for a device to attach before driving the bus
 * reset.
 *
 * @param[in] speed Which controller (FS or HS).
 * @return The two-bit line state.
 * @retval 0 Nothing attached (or invalid speed / unpowered module).
 * @retval 1 A device's D+ pull-up is visible.
 * @pre `ra8_usb_host_init` ran for a meaningful read.
 * @pre The controller is clocked.
 * @post No state is modified.
 * @post The value is a single-moment snapshot.
 * @note Safe to call from any context.
 * @since 0.1.0
 */
uint16_t ra8_usb_host_line_state(ra8_usb_speed_t speed);

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
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              The DCP now targets @p dev_addr.
 * @retval k_ra8_err_invalid_arg `speed` or `dev_addr` out of range.
 *
 * @pre `ra8_usb_host_init` ran; the bus reset completed (RHST valid).
 * @pre No control transfer is in flight on the DCP.
 *
 * @post `DCPMAXP.DEVSEL` equals @p dev_addr with MXPS preserved.
 * @post The DEVADDn slot for @p dev_addr carries the link speed.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_set_target(ra8_usb_speed_t speed, uint8_t dev_addr);

/**
 * @brief Configure a controller pipe against an attached device's bulk endpoint.
 *
 * @details
 * Host-mode counterpart of `ra8_usb_configure_endpoint`: programs
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
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              Pipe configured, DATA0 forced, parked NAK.
 * @retval k_ra8_err_invalid_arg An argument is out of range.
 *
 * @pre `ra8_usb_host_init` ran and SET_CONFIGURATION reset the endpoint.
 * @pre The pipe is not currently armed (no transfer in flight).
 *
 * @post The pipe targets @p dev_addr endpoint @p ep_num with PID = NAK.
 * @post The pipe's BRDY/NRDY/BEMP status bits are cleared.
 *
 * @note Not thread-safe.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_pipe_setup(ra8_usb_speed_t speed,
                                                uint8_t         pipe_num,
                                                uint8_t         dev_addr,
                                                uint8_t         ep_num,
                                                bool            device_to_host,
                                                uint16_t        max_packet);

/**
 * @brief Transmit one bulk packet to the device and wait for the ACK.
 *
 * @details
 * Single-packet host bulk OUT: stages @p data into the pipe buffer
 * (FRDY-gated FIFO write + BVAL + PID=BUF via `ra8_usb_queue_in`), then
 * waits for the buffer-empty (BEMP) event that marks the packet on the
 * wire and acknowledged, and parks the pipe NAK. Sized for protocol
 * headers such as the 31-byte MSC CBW; @p len must not exceed the pipe's
 * max packet size.
 *
 * @param[in] speed    Which controller (FS or HS).
 * @param[in] pipe_num Pipe configured by `ra8_usb_host_pipe_setup` (OUT).
 * @param[in] data     Packet payload.
 * @param[in] len      Payload length in bytes (<= pipe MPS).
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok              Packet transmitted and ACKed.
 * @retval k_ra8_err_invalid_arg `speed` / `pipe_num` / `len` out of range.
 * @retval k_ra8_err_null_ptr    `data` was NULL.
 * @retval k_ra8_err_hw_error    The device STALLed the endpoint.
 * @retval k_ra8_err_hw_timeout  No ACK before the spin bound.
 *
 * @pre The pipe was configured by `ra8_usb_host_pipe_setup`.
 * @pre The device is configured (SET_CONFIGURATION accepted).
 *
 * @post The pipe PID is NAK and its BEMP status is cleared.
 * @post On k_ra8_ok the device has ACKed the packet.
 *
 * @note Blocking; bounded by an internal spin limit.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t
ra8_usb_host_bulk_out(ra8_usb_speed_t speed, uint8_t pipe_num, const uint8_t* data, uint16_t len);

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
 * @param[in]  pipe_num     Pipe configured by `ra8_usb_host_pipe_setup` (IN).
 * @param[out] buf          Destination buffer.
 * @param[in]  max_len      Capacity of @p buf in bytes.
 * @param[out] out_received Receives the byte count gathered.
 *
 * @return `ra8_err_t` error code.
 * @retval k_ra8_ok                Transfer ended (short packet or count).
 * @retval k_ra8_err_invalid_arg   `speed` / `pipe_num` out of range.
 * @retval k_ra8_err_null_ptr      `buf` or `out_received` was NULL.
 * @retval k_ra8_err_invalid_state The pipe has no max-packet programmed.
 * @retval k_ra8_err_hw_error      The device STALLed the endpoint.
 * @retval k_ra8_err_hw_timeout    No packet before the spin bound.
 *
 * @pre The pipe was configured by `ra8_usb_host_pipe_setup`.
 * @pre The device is configured and expecting to send (e.g. post-CBW).
 *
 * @post The pipe PID is NAK; `*out_received` holds the byte count.
 * @post On error the partial count gathered so far is still reported.
 *
 * @note Blocking; bounded by an internal spin limit per packet.
 * @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_usb_host_bulk_in(ra8_usb_speed_t speed,
                                             uint8_t         pipe_num,
                                             uint8_t*        buf,
                                             uint16_t        max_len,
                                             uint16_t*       out_received);

#ifdef __cplusplus
}
#endif
