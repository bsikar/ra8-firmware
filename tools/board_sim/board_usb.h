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

#ifdef __cplusplus
}
#endif
