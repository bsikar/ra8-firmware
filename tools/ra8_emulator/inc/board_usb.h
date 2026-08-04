/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_usb.h
 * @brief USBFS controller model + a virtual USB host for the board emulator
 *
 * @details
 * Models the RA8D2 USB 2.0 Full-Speed controller (USBFS, base 0x40250000,
 * HUM Ch 36) at register granularity AND drives a virtual USB host that runs
 * the standard chapter-9 enumeration against the device-side firmware. This is
 * the "debug USB without hardware" peripheral: the real, unmodified ThreadX +
 * Eclipse USBX CDC-ACM firmware (port/usbx/ux_dcd_ra8_usb -> libs/ra8_hal
 * ra8_usb*.c) runs on the emulated Cortex-M, and this module presents it a host
 * that issues GET_DESCRIPTOR / SET_ADDRESS / SET_CONFIGURATION / CDC requests,
 * clocks the device's descriptor responses out of the CFIFO, and advances the
 * device state (DVSQ powered -> default -> address -> configured) until USBX's
 * CDC-ACM activate callback fires.
 *
 * Two cooperating halves live here:
 *
 *  1. **Controller model** -- SYSCFG (DPRPU pull-up / USBE / SCKE), INTSTS0
 *     (CTSQ control-stage, DVSQ device-state, VALID, CTRT / DVST / BRDY event
 *     bits), the CFIFO data port with its CFIFOSEL / CFIFOCTR handshake
 *     (FRDY / BVAL / BCLR / DTLN), USBREQ..USBLENG SETUP latches, DCPCTR
 *     (PID / CCPL), the PIPECTR / BRDYSTS / NRDYSTS pipe machinery, and
 *     USBADDR / FRMNUM. Reads / writes are dispatched from board_periph's
 *     MMIO callbacks (the USB window is forwarded to ::board_usb_read /
 *     ::board_usb_write).
 *
 *  2. **Virtual host** -- a small state machine, stepped once per emulation
 *     chunk from ::board_usb_tick, that watches SYSCFG.DPRPU, drives a bus
 *     reset, and walks the SETUP sequence. Each control-read SETUP is latched
 *     into the controller, the device's CTRT interrupt is raised through the
 *     ICU -> NVIC path (so the real ISR ``ra8_usb_dispatch`` runs), and the
 *     descriptor bytes the device pushes into the DCP FIFO are drained back as
 *     the host's IN data before the status stage is delivered.
 *
 * Design: this module owns no Unicorn engine and no AppKit dependency. The
 * engine is passed in where the model must read / write emulated memory or
 * raise an NVIC line; the NVIC pend itself is delegated to board_periph (which
 * owns the ICU IELSR table and the IRQ ring) through ::board_usb_set_irq_raiser
 * so all exception delivery stays in one place.
 *
 *
 * @since 0.1.0
 */

#pragma once

#include <stdint.h>
#include <unicorn/unicorn.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Signature of the ICU event-raise hook board_periph installs.
 *
 * @details The USB host model raises the USBFS controller interrupt by
 * asserting its ELC event (0x09A); board_periph owns the IELSR event-link
 * table, the NVIC enable shadow and the pending-IRQ ring, so it supplies this
 * callback and the USB model never touches that state directly.
 *
 * @param[in,out] uc    Unicorn engine (the ICU reads IELSR / NVIC from PPB).
 * @param[in]     event ELC event number to assert (USBFS_INT == 0x09A).
 * @since 0.1.0
 */
typedef void (*board_usb_irq_raiser_t)(uc_engine* uc, uint16_t event);

/**
 * @brief Reset the USBFS controller model and the virtual host state machine.
 *
 * @details Clears every modelled register, the CFIFO staging buffers, the host
 * enumeration step machine and the observability counters. Call once after the
 * memory map is created and before the run loop.
 *
 * @param[in] trace When true, each enumeration step and raised USB interrupt is
 *                  logged to stderr as it happens (the --trace flag).
 * @return Nothing.
 * @post The model is in its power-on reset state; the host is idle, waiting for
 *       the firmware to assert SYSCFG.DPRPU.
 * @since 0.1.0
 */
void board_usb_init(bool trace);

/**
 * @brief Install the ICU event-raise hook used to pend the USBFS interrupt.
 *
 * @param[in] raise Callback board_periph supplies to assert an ELC event, or
 *                  NULL to detach (the host then cannot raise USB IRQs).
 * @return Nothing.
 * @post Subsequent host steps raise USBFS_INT through @p raise.
 * @since 0.1.0
 */
void board_usb_set_irq_raiser(board_usb_irq_raiser_t raise);

/**
 * @brief Dispatch an MMIO read inside the USBFS register window.
 *
 * @param[in,out] uc      Unicorn engine (unused today; kept for symmetry).
 * @param[in]     addr    Absolute peripheral address being read.
 * @param[in]     size    Access width in bytes (1 / 2 / 4).
 * @param[out]    handled True iff @p addr is inside the USBFS window.
 * @return The register value when @p *handled is true, else 0.
 * @since 0.1.0
 */
uint64_t board_usb_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled);

/**
 * @brief Dispatch an MMIO write inside the USBFS register window.
 *
 * @param[in,out] uc      Unicorn engine (unused today; kept for symmetry).
 * @param[in]     addr    Absolute peripheral address being written.
 * @param[in]     size    Access width in bytes (1 / 2 / 4).
 * @param[in]     value   Value being written.
 * @param[out]    handled True iff @p addr is inside the USBFS window.
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_write(uc_engine* uc, uint64_t addr, unsigned size, uint64_t value, bool* handled);

/**
 * @brief Advance the virtual USB host one emulation chunk.
 *
 * @details Stepped once per run-loop chunk (the SysTick cadence). The host
 * polls for the device pull-up, performs the bus reset, and clocks the
 * chapter-9 SETUP sequence forward one micro-step per call -- delivering a
 * SETUP and raising CTRT, waiting for the device's descriptor response or
 * status-stage completion, then advancing. Idle once the device is configured
 * (and, with bulk data queued, while echo traffic is in flight).
 *
 * @param[in,out] uc Unicorn engine (the model reads back the device's CFIFO /
 *                   DCPCTR writes from its own state and pends the USB IRQ
 *                   through the installed raiser).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_tick(uc_engine* uc);

/**
 * @brief Report whether enumeration reached the CONFIGURED state.
 *
 * @return true once the host has issued SET_CONFIGURATION and the controller's
 *         DVSQ has advanced to Configured.
 * @since 0.1.0
 */
bool board_usb_configured(void);

/**
 * @brief One-line, human-readable USB device state for the board view.
 *
 * @details Returns a short static string describing the live enumeration state
 * of the modelled USBFS device -- the INTSTS0.DVSQ stage name (@c "Powered" /
 * @c "Default" / @c "Address" / @c "Configured" / @c "Suspended"), upgraded to
 * @c "CONFIGURED (CDC-ACM active)" once SET_CONFIGURATION has completed and
 * USBX's CDC-ACM activate callback has fired. ra8_emulator's graphical board view
 * shows this verbatim on its "USB:" status line so a non-display USB example
 * (e.g. threadx_usbx_cdc_demo) is observable as it enumerates. The returned
 * pointer is to static storage and must not be freed; it is valid until the
 * next call.
 *
 * @return NUL-terminated state string (never NULL).
 * @since 0.1.0
 */
const char* board_usb_state_string(void);

/**
 * @brief Queue host->device bulk bytes for the CDC data OUT pipe (echo test).
 *
 * @details Used by the secondary CDC-echo check: once the device is configured
 * the host delivers these bytes to the bulk OUT pipe and then reads the bytes
 * the device echoes back on the bulk IN pipe. Bytes beyond the staging
 * capacity are dropped.
 *
 * @param[in] data Source bytes (copied); ignored if NULL.
 * @param[in] len  Number of bytes to queue.
 * @return Nothing.
 * @post Up to the staging capacity of @p data is delivered after CONFIGURED.
 * @since 0.1.0
 */
void board_usb_feed_bulk_in(const uint8_t* data, uint32_t len);

/**
 * @brief Number of bulk bytes the host has read back as the device's echo.
 *
 * @return Count of echoed bytes received on the bulk IN pipe.
 * @since 0.1.0
 */
uint32_t board_usb_echo_received(void);

/**
 * @brief Print the USB section of the end-of-run summary.
 *
 * @details Reports each enumeration step taken (host SETUP -> device stage), a
 * clear "USB: device CONFIGURED (CDC-ACM active)" marker when reached, the
 * final device state and USB interrupt count, and -- when bulk echo was driven
 * -- the OUT / echoed-IN byte totals.
 *
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_report(void);

/* =============================================================================
 * Chip-internal USBHS-host <-> USBFS-device bridge.
 *
 * These entry points let the modelled on-chip USBHS host controller
 * (board_periph_usbhs_host.c, register window 0x40351000) drive THIS USBFS
 * device model directly, closing the RA8D2's chip-internal self-loop (the two
 * jacks cabled to each other on the bench). When an external real-firmware host
 * is present the built-in virtual chapter-9 host stands down (::board_usb_tick
 * returns early) so exactly one host drives the device: the real firmware, via
 * these primitives, exactly as the physical port-to-port cable does.
 * =============================================================================
 */

/**
 * @brief Declare (or withdraw) a real-firmware USB host driving the device.
 *
 * @details When @p present is true the built-in virtual chapter-9 host stops
 * driving the device from ::board_usb_tick, because the emulated USBHS host
 * controller (board_periph_usbhs_host.c) now enumerates and talks to the device
 * through the bridge primitives below. Set once at startup for a self-loop app.
 *
 * @param[in] present true to hand the device over to the external host bridge.
 * @return Nothing.
 * @post ::board_usb_tick no longer advances the built-in virtual host.
 * @since 0.1.0
 */
void board_usb_set_external_host(bool present);

/**
 * @brief Whether the modelled USBFS device has asserted its D+ pull-up (DPRPU).
 *
 * @return true once the device firmware called ra8_usb_device_attach (SYSCFG.DPRPU
 *         set), i.e. a device is electrically present for the host to enumerate.
 * @since 0.1.0
 */
bool board_usb_dev_attached(void);

/**
 * @brief Bridge a host bus reset onto the device (advance DVSQ to Default).
 *
 * @details Mirrors the physical USB reset the host drives on DVSTCTR0.USBRST:
 * returns the device to the Default state and raises its DVST interrupt so the
 * device firmware re-arms its DCP, exactly as the built-in virtual host's reset
 * phase does.
 *
 * @param[in,out] uc Unicorn engine (to pend the device USB interrupt).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_bridge_bus_reset(uc_engine* uc);

/**
 * @brief Deliver an 8-byte host SETUP packet to the device (raise its CTRT).
 *
 * @details Latches the packet into the device's USBREQ..USBLENG mirror
 * registers, sets the control-transfer stage (CTSQ) from the request direction,
 * and raises the device CTRT interrupt so the real device ISR decodes it. A
 * SET_ADDRESS additionally applies the SIE side effect the hardware performs
 * (USBADDR latched, DVSQ -> Address).
 *
 * @param[in,out] uc    Unicorn engine (to pend the device USB interrupt).
 * @param[in]     setup Eight SETUP bytes (bmRequestType, bRequest, wValue,
 *                      wIndex, wLength, little-endian) to deliver.
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_bridge_deliver_setup(uc_engine* uc, const uint8_t* setup);

/**
 * @brief Whether the device has queued a control-IN response on the DCP.
 *
 * @return true when the device armed its DCP (PID=BUF) with valid IN data for
 *         the host to read (e.g. a GET_DESCRIPTOR response).
 * @since 0.1.0
 */
bool board_usb_bridge_dcp_in_ready(void);

/**
 * @brief Consume the device's queued control-IN response into @p buf.
 *
 * @param[out] buf Destination for the device's DCP IN bytes.
 * @param[in]  cap Capacity of @p buf in bytes.
 * @return The number of bytes copied (0 if none were queued).
 * @post The device DCP IN buffer is cleared and ready for the next transfer.
 * @since 0.1.0
 */
uint16_t board_usb_bridge_dcp_in_take(uint8_t* buf, uint16_t cap);

/**
 * @brief Drive the read-status stage of a host control-read (raise CTRT).
 *
 * @details Advertises the read status stage (CTSQ) and raises the device CTRT
 * so the device firmware closes its side of a completed control-read transfer.
 *
 * @param[in,out] uc Unicorn engine (to pend the device USB interrupt).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_bridge_ctrl_status(uc_engine* uc);

/**
 * @brief Take (consume) the device's control-transfer completion (DCPCTR.CCPL).
 *
 * @details The device firmware pulses DCPCTR.CCPL once it has finished
 * processing a no-data host-to-device control transfer (e.g. after
 * SET_CONFIGURATION has activated its class and armed the class endpoints).
 * The USBHS host model gates a no-data control-write status stage on this so
 * the polled host does not race past a request the device is still applying --
 * the same completion the built-in virtual host waits on. Returns true exactly
 * once per device CCPL pulse, then clears it so the next transfer starts clean.
 *
 * @return true if the device had pulsed CCPL since the last call, else false.
 * @post Any observed CCPL latch is cleared.
 * @since 0.1.0
 */
bool board_usb_bridge_dev_took_ccpl(void);

/**
 * @brief Mark the device configured after a host SET_CONFIGURATION.
 *
 * @param[in,out] uc Unicorn engine (to pend the device USB interrupt).
 * @return Nothing.
 * @post The device DVSQ reaches Configured and DVST is raised.
 * @since 0.1.0
 */
void board_usb_bridge_mark_configured(uc_engine* uc);

/**
 * @brief Deliver a host bulk-OUT packet to a device endpoint pipe.
 *
 * @details Stages @p len bytes into the device's OUT pipe buffer and raises the
 * pipe's BRDY interrupt, so the device firmware's bulk-OUT receive path (e.g.
 * the CDC-ACM read, or the MSC CBW/data receive) completes.
 *
 * @param[in,out] uc       Unicorn engine (to pend the device USB interrupt).
 * @param[in]     dev_pipe Device pipe number the endpoint maps to (1..9).
 * @param[in]     data     Payload bytes (host -> device).
 * @param[in]     len      Payload length in bytes.
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_bridge_bulk_out(uc_engine* uc, uint8_t dev_pipe, const uint8_t* data, uint16_t len);

/**
 * @brief Deliver a host control-write data-stage packet to the device DCP.
 *
 * @details Stages @p len bytes of a control-OUT data stage (e.g. a DFU_DNLOAD
 * firmware block) into the device's DCP (EP0) OUT buffer and raises the device
 * DCP BRDY so the device firmware's control-OUT receive path drains it. The
 * companion pump in ::board_usb_tick re-asserts BRDY until the device -- which
 * arms its DCP off its own IRQ -- has actually received the packet, mirroring
 * the SIE's OUT-token retry on real silicon.
 *
 * @param[in,out] uc   Unicorn engine (to pend the device USB interrupt).
 * @param[in]     data Payload bytes (host -> device); ignored if NULL.
 * @param[in]     len  Payload length in bytes.
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_bridge_dcp_out(uc_engine* uc, const uint8_t* data, uint16_t len);

/**
 * @brief Whether the device has drained the last host DCP control-OUT packet.
 *
 * @details The USBHS host model gates a control-write data stage's buffer-empty
 * (BEMP) completion on this, exactly as it does for a bulk-OUT pipe, so the
 * polled host hands the device one control-OUT packet at a time.
 *
 * @return true once the device has fully drained the staged DCP OUT packet.
 * @since 0.1.0
 */
bool board_usb_bridge_dcp_out_consumed(void);

/**
 * @brief Whether the device has drained the last host bulk-OUT packet.
 *
 * @details The USBHS host model gates a bulk-OUT pipe's buffer-empty (BEMP)
 * completion on this so a multi-packet data stage (e.g. a WRITE(10) payload)
 * hands the device one packet at a time: the polled host does not push the
 * next OUT packet until the device firmware has drained the current one from
 * its CFIFO, otherwise the fresh packet would overwrite the undrained bank.
 *
 * @param[in] dev_pipe Device pipe number the endpoint maps to (1..9).
 * @return true once the device has fully drained the staged OUT packet.
 * @since 0.1.0
 */
bool board_usb_bridge_bulk_out_consumed(uint8_t dev_pipe);

/**
 * @brief Whether the device has queued a bulk-IN packet on @p dev_pipe.
 *
 * @param[in] dev_pipe Device pipe number to probe (1..9).
 * @return true when the device has staged bytes for the host to read.
 * @since 0.1.0
 */
bool board_usb_bridge_bulk_in_ready(uint8_t dev_pipe);

/**
 * @brief Consume a device bulk-IN packet from @p dev_pipe into @p buf.
 *
 * @details Copies the device's queued IN bytes out and raises the pipe's BEMP
 * interrupt so the device firmware can stage the next packet.
 *
 * @param[in,out] uc       Unicorn engine (to pend the device USB interrupt).
 * @param[in]     dev_pipe Device pipe number to drain (1..9).
 * @param[out]    buf      Destination buffer.
 * @param[in]     cap      Capacity of @p buf in bytes.
 * @return The number of bytes copied (0 if none were queued).
 * @since 0.1.0
 */
uint16_t board_usb_bridge_bulk_in_take(uc_engine* uc, uint8_t dev_pipe, uint8_t* buf, uint16_t cap);

/* =============================================================================
 * Self-loop role routing (Config A vs Config B).
 *
 * The two on-chip controllers can run the self-loop in either polarity: HS
 * host + FS device ("Config A", the default) or FS host + HS device ("Config
 * B", dfu_selftest_fs_host). On silicon each controller's role is selected by
 * the firmware itself through SYSCFG: DCFM=1 declares controller (host) mode
 * (HUM Ch 36.2.1 / 37.2.1), DPRPU=1 declares the device-role D+ pull-up. The
 * models mirror that: a DCFM=1 write to the USBFS window, or a DPRPU=1 write
 * to the USBHS window, swaps the window<->model binding so the single host
 * model serves the window the firmware drives as host and the single device
 * model serves the other -- no per-app configuration, the register writes
 * decide, exactly as the silicon does.
 * =============================================================================
 */

/**
 * @brief Whether the self-loop window roles are swapped (Config B).
 *
 * @details false (default): USBFS window = device model, USBHS window = host
 * model (Config A). true: the firmware declared the opposite polarity (FS
 * SYSCFG.DCFM=1 or HS SYSCFG.DPRPU=1), so the USBFS window routes to the host
 * model and the USBHS window routes to the device model (Config B).
 *
 * @return true when the bindings are swapped for this run.
 * @pre None; safe at any time (reset to false by ::board_usb_init).
 * @post No model state is modified (read-only accessor).
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usb_roles_swap
 * @since 0.1.0
 */
bool board_usb_roles_swapped(void);

/**
 * @brief Swap the self-loop window<->model bindings (enter Config B).
 *
 * @details One-shot and sticky until ::board_usb_init: migrates the USBHS
 * window's accumulated register shadow out of the host model into the device
 * model (the device firmware's PHY + module-init writes landed there before
 * its role became knowable), resets the host model for its fresh life behind
 * the USBFS window, and retargets the device interrupt at the USBHS ICU event
 * (USBHS_USB_INT_RESUME) instead of USBFS_INT. Idempotent: a second call in
 * the same run is a no-op.
 *
 * @param[in,out] uc Unicorn engine (unused today; kept for symmetry with the
 *                   other bridge mutators, which pend device IRQs through it).
 * @return Nothing.
 * @pre ::board_usb_set_external_host declared the self-loop bridge active.
 * @pre No USB traffic has crossed the loop yet (both roles still initializing).
 * @post ::board_usb_roles_swapped reports true.
 * @post The device model raises the USBHS interrupt event from now on.
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usb_roles_swapped
 * @since 0.1.0
 */
void board_usb_roles_swap(uc_engine* uc);

/**
 * @brief Device-model register read by window byte offset (role routing).
 *
 * @details Offset-addressed twin of the USBFS-window claim inside
 * ::board_usb_read, exported so the USBHS window block can route its reads
 * into the device model when the roles are swapped (Config B: the DCD drives
 * the USBHS controller in device role).
 *
 * @param[in,out] uc   Unicorn engine (unused; kept for handler symmetry).
 * @param[in]     off  Byte offset inside the controller register window
 *                     (0 .. 0xFE; the device model spans 0x100 bytes).
 * @param[in]     size Access width in bytes (1 or 2).
 * @return The register value, zero-extended to 64 bits.
 * @pre @p off is inside the modelled 0x100-byte device register span.
 * @pre ::board_usb_init has run (model state is live).
 * @post Read side effects (CFIFO drain cursors) match the device model's.
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usb_dev_reg_write
 * @since 0.1.0
 */
uint64_t board_usb_dev_reg_read(uc_engine* uc, uint64_t off, unsigned size);

/**
 * @brief Device-model register write by window byte offset (role routing).
 *
 * @details Offset-addressed twin of the USBFS-window claim inside
 * ::board_usb_write, exported so the USBHS window block can route its writes
 * into the device model when the roles are swapped (Config B).
 *
 * @param[in,out] uc    Unicorn engine (unused; kept for handler symmetry).
 * @param[in]     off   Byte offset inside the controller register window
 *                      (0 .. 0xFE; the device model spans 0x100 bytes).
 * @param[in]     size  Access width in bytes (1 or 2).
 * @param[in]     value Value the firmware wrote.
 * @return Nothing.
 * @pre @p off is inside the modelled 0x100-byte device register span.
 * @pre ::board_usb_init has run (model state is live).
 * @post Write side effects (CFIFO staging, W0C status) match the device model's.
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usb_dev_reg_read
 * @since 0.1.0
 */
void board_usb_dev_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value);

/**
 * @brief Host-model register read by window byte offset (role routing).
 *
 * @details Exported by board_periph_usbhs_host.c so the USBFS window claim in
 * board_usb.c can route its reads into the host model when the roles are
 * swapped (Config B: the polled ra8_usb_host_* driver runs on the USBFS
 * controller).
 *
 * @param[in,out] uc   Unicorn engine (bulk/control latching pends device IRQs).
 * @param[in]     off  Byte offset inside the controller register window.
 * @param[in]     size Access width in bytes (1, 2 or 4).
 * @return The register value, zero-extended to 64 bits.
 * @pre @p off is inside the host model's 0x200-byte register span.
 * @pre The self-loop bridge is active (--usbhs-loop).
 * @post Read side effects (status latch, FIFO cursors) match the host model's.
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usbhs_host_reg_write
 * @since 0.1.0
 */
uint64_t board_usbhs_host_reg_read(uc_engine* uc, uint64_t off, unsigned size);

/**
 * @brief Host-model register write by window byte offset (role routing).
 *
 * @details Exported by board_periph_usbhs_host.c so the USBFS window claim in
 * board_usb.c can route its writes into the host model when the roles are
 * swapped (Config B).
 *
 * @param[in,out] uc    Unicorn engine (transfer launches drive the device model).
 * @param[in]     off   Byte offset inside the controller register window.
 * @param[in]     size  Access width in bytes (1, 2 or 4).
 * @param[in]     value Value the firmware wrote.
 * @return Nothing.
 * @pre @p off is inside the host model's 0x200-byte register span.
 * @pre The self-loop bridge is active (--usbhs-loop).
 * @post Write side effects (SETUP launch, FIFO commits) match the host model's.
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usbhs_host_reg_read
 * @since 0.1.0
 */
void board_usbhs_host_reg_write(uc_engine* uc, uint64_t off, unsigned size, uint64_t value);

/**
 * @brief Hand the host model's register shadow off and reset the host model.
 *
 * @details Role-swap migration primitive (board_periph_usbhs_host.c): copies
 * up to @p word_capacity 16-bit shadow words -- the USBHS window's accumulated
 * writes, i.e. the device firmware's PHY + module-init register state -- into
 * @p dst_words, then resets the entire host model so it starts virgin behind
 * the USBFS window. Called exactly once, by ::board_usb_roles_swap.
 *
 * @param[out] dst_words     Destination for the 16-bit shadow words.
 * @param[in]  word_capacity Capacity of @p dst_words in 16-bit words.
 * @return The number of 16-bit words copied.
 * @pre @p dst_words is non-NULL with room for @p word_capacity words.
 * @pre No host-side USB traffic has occurred yet (bring-up phase only).
 * @post The host model is fully reset (power-on state).
 * @note Not thread-safe; single-threaded emulation loop use.
 * @see board_usb_roles_swap
 * @since 0.1.0
 */
uint32_t board_usbhs_host_shadow_handoff(uint16_t* dst_words, uint32_t word_capacity);

/* =============================================================================
 * Loop-cable transport -- consumed by the USBHS HOST-mode model
 * (board_usb_host.c) when the firmware itself is the host on the self-loop
 * bench (USBHS J7 host cabled to USBFS J11 device). Each call is one bus
 * operation the firmware host performs against the modelled USBFS device.
 * =============================================================================
 */

/**
 * @brief Latch the self-loop: the firmware brought a controller up as HOST.
 *
 * @details Called by the USBHS host model when the firmware selects host mode
 * (SYSCFG.DCFM on the HS instance). From then on the bench topology is the
 * loop cable -- the firmware host owns the bus -- so the built-in virtual host
 * parks for the remainder of the run (its tick returns immediately) instead of
 * competing for the device's control pipe and stealing bulk-IN data. One-way
 * for the run; a later host deinit leaves the cable in place, exactly like the
 * physical bench.
 *
 * @return Nothing.
 * @post ::board_usb_tick is inert; the loop calls below drive the device.
 * @since 0.1.0
 */
void board_usb_loop_latch(void);

/**
 * @brief Whether the modelled USBFS device presents its D+ pull-up.
 *
 * @return true when the device firmware set SYSCFG.DPRPU (attach), else false.
 * @since 0.1.0
 */
bool board_usb_loop_attached(void);

/**
 * @brief Deliver a bus reset to the modelled USBFS device (host released RST).
 *
 * @details Drops any staged control / bulk data (a bus reset empties the
 * FIFOs), forces the device state to Default, and raises the DVST interrupt so
 * the device firmware re-arms its DCP -- the same sequence the built-in
 * virtual host performs at the start of its enumeration.
 *
 * @param[in,out] uc Unicorn engine (to pend the device's USB interrupt).
 * @return Nothing.
 * @post Device DVSQ = Default; stale staging buffers are cleared.
 * @since 0.1.0
 */
void board_usb_loop_bus_reset(uc_engine* uc);

/**
 * @brief Deliver one SETUP packet from the firmware host to the device.
 *
 * @details Latches the four SETUP half-words into USBREQ..USBLENG, marks the
 * SETUP valid with the matching control-stage code (CTSQ), and raises the
 * device's CTRT interrupt. SET_ADDRESS is applied SIE-style (USBADDR latch +
 * DVSQ Address + DVST) exactly as the built-in host does; SET_CONFIGURATION
 * arms the configured-state transition for the device's CCPL.
 *
 * @param[in,out] uc   Unicorn engine (to pend the device's USB interrupt).
 * @param[in]     req  USBREQ word (bmRequestType | bRequest << 8).
 * @param[in]     val  wValue.
 * @param[in]     indx wIndex.
 * @param[in]     leng wLength.
 * @return true when the device SIE completes the whole transfer itself
 *         (SET_ADDRESS): the host's status stage needs no device CCPL.
 * @retval true  SIE-handled request; treat the status stage as already done.
 * @retval false Normal request; the device firmware will end it with CCPL.
 * @since 0.1.0
 */
bool board_usb_loop_setup(uc_engine* uc, uint16_t req, uint16_t val, uint16_t indx, uint16_t leng);

/**
 * @brief Poll the device's control-transfer completion (DCPCTR.CCPL).
 *
 * @details Observes -- and consumes -- the device firmware's CCPL write, which
 * on hardware makes the device SIE hand the host its status-stage ZLP. When
 * the completed request was SET_CONFIGURATION the device state advances to
 * Configured (DVST raised), mirroring the built-in host's flow.
 *
 * @param[in,out] uc Unicorn engine (to pend the device's USB interrupt).
 * @return true when the device had asserted CCPL since the last poll.
 * @retval true  Transfer complete; the host's status ZLP is available.
 * @retval false No completion yet (device firmware still processing).
 * @since 0.1.0
 */
bool board_usb_loop_take_ccpl(uc_engine* uc);

/**
 * @brief Control-IN bytes the device has queued and the host not yet drained.
 *
 * @return Byte count remaining in the device's DCP IN staging (0 when none is
 *         committed).
 * @since 0.1.0
 */
uint16_t board_usb_loop_ctrl_in_avail(void);

/**
 * @brief Drain up to @p cap control-IN bytes from the device's DCP staging.
 *
 * @details The host-side read cursor advances; once the staging is fully
 * drained it is released so the device can queue the next response.
 *
 * @param[out] dst Destination buffer.
 * @param[in]  cap Capacity of @p dst in bytes.
 * @return Number of bytes copied (0 when nothing is staged).
 * @since 0.1.0
 */
uint16_t board_usb_loop_ctrl_in_read(uint8_t* dst, uint16_t cap);

/**
 * @brief Drop whatever remains of the device's control-IN staging.
 *
 * @details The host-side equivalent of a DCP read-window BCLR: any undrained
 * response bytes are discarded and the staging is released.
 *
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_loop_ctrl_in_flush(void);

/**
 * @brief Deliver a control-OUT data-stage packet to the device's DCP.
 *
 * @details Stages the bytes in the device's DCP OUT buffer and raises the
 * device's DCP BRDY -- the packet a device-side ``ra8_usb_dcp_out_read`` then
 * drains.
 *
 * @param[in,out] uc   Unicorn engine (to pend the device's USB interrupt).
 * @param[in]     data Payload bytes (host to device).
 * @param[in]     len  Payload length in bytes (clamped to the staging size).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_loop_ctrl_out(uc_engine* uc, const uint8_t* data, uint16_t len);

/**
 * @brief Run the control-read status stage against the device (host OUT ZLP).
 *
 * @details Advances the device's control stage to "read status" and raises
 * CTRT so the device firmware completes the transfer with CCPL -- what the
 * host's zero-length status OUT causes on hardware.
 *
 * @param[in,out] uc Unicorn engine (to pend the device's USB interrupt).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_loop_status_out_zlp(uc_engine* uc);

/**
 * @brief Deliver one bulk-OUT packet from the firmware host to a device pipe.
 *
 * @details Stages the packet in the device pipe's OUT buffer and raises the
 * pipe's BRDY, mirroring the built-in host's echo path. The device pipe is
 * addressed by ENDPOINT number: the ``ux_dcd_ra8_usb`` bridge maps device
 * endpoint n onto controller pipe n, the same fixed mapping the built-in
 * virtual host encodes.
 *
 * @param[in,out] uc   Unicorn engine (to pend the device's USB interrupt).
 * @param[in]     ep   Device endpoint number (1..9).
 * @param[in]     data Packet bytes.
 * @param[in]     len  Packet length (clamped to the pipe staging size).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_loop_bulk_out(uc_engine* uc, uint8_t ep, const uint8_t* data, uint16_t len);

/**
 * @brief Bulk-IN bytes a device pipe has queued and the host not yet drained.
 *
 * @param[in] ep Device endpoint number (1..9).
 * @return Byte count remaining in that pipe's IN staging (0 when none).
 * @since 0.1.0
 */
uint16_t board_usb_loop_bulk_in_avail(uint8_t ep);

/**
 * @brief Drain up to @p cap bulk-IN bytes from a device pipe's staging.
 *
 * @details Once the staging is fully drained the device's BEMP for that pipe
 * is raised (transmit buffer empty) so the device firmware can queue the next
 * packet -- exactly what the built-in host's echo reader does.
 *
 * @param[in,out] uc  Unicorn engine (to pend the device's USB interrupt).
 * @param[in]     ep  Device endpoint number (1..9).
 * @param[out]    dst Destination buffer.
 * @param[in]     cap Capacity of @p dst in bytes.
 * @return Number of bytes copied (0 when nothing is staged).
 * @since 0.1.0
 */
uint16_t board_usb_loop_bulk_in_read(uc_engine* uc, uint8_t ep, uint8_t* dst, uint16_t cap);

/**
 * @brief Drop whatever remains of a device pipe's bulk-IN staging.
 *
 * @param[in] ep Device endpoint number (1..9).
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_loop_bulk_in_flush(uint8_t ep);

#ifdef __cplusplus
}
#endif
