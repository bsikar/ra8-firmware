/**
 * @file board_usb.h
 * @brief USBFS controller model + a virtual USB host for the board emulator
 *
 * @details
 * Models the RA8D2 USB 2.0 Full-Speed controller (USBFS, base 0x40250000,
 * HUM Ch 36) at register granularity AND drives a virtual USB host that runs
 * the standard chapter-9 enumeration against the device-side firmware. This is
 * the "debug USB without hardware" peripheral: the real, unmodified ThreadX +
 * Eclipse USBX CDC-ACM firmware (port/usbx/ux_dcd_ra_usb -> libs/ra_hal
 * ra_usb*.c) runs on the emulated Cortex-M, and this module presents it a host
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
 *     ICU -> NVIC path (so the real ISR ``ra_usb_dispatch`` runs), and the
 *     descriptor bytes the device pushes into the DCP FIFO are drained back as
 *     the host's IN data before the status stage is delivered.
 *
 * Design: this module owns no Unicorn engine and no AppKit dependency. The
 * engine is passed in where the model must read / write emulated memory or
 * raise an NVIC line; the NVIC pend itself is delegated to board_periph (which
 * owns the ICU IELSR table and the IRQ ring) through ::board_usb_set_irq_raiser
 * so all exception delivery stays in one place.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
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
 * USBX's CDC-ACM activate callback has fired. board_sim's graphical board view
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
 * @return true once the device firmware called ra_usb_device_attach (SYSCFG.DPRPU
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

#ifdef __cplusplus
}
#endif
