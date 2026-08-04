/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file board_usb_host.h
 * @brief USBHS HOST-mode controller model (self-loop peer of the USBFS device)
 *
 * @details
 * Models the RA8D2 USB 2.0 High-Speed controller (USBHS, base 0x40351000,
 * HUM Ch 37) in HOST role at register granularity, for firmware that is ITSELF
 * the bus host on the self-loop bench: USBHS (J7) host cabled to USBFS (J11)
 * device, both driven by the same chip (usb_selftest_cdc's topology, and the
 * TrustZone NS image of tz_nsc_cgc_usb). The registers the polled first-party
 * host engine (`ra8_usb_host_*` in libs/ra8_hal) drives -- SYSCFG / DVSTCTR0 /
 * DCPCTR.SUREQ / USBREQ..USBLENG / CFIFO / BRDYSTS / BEMPSTS / INTSTS1 SACK
 * / the PIPESEL window -- are serviced here, and every bus transaction is
 * forwarded over the loop-cable transport (board_usb.h `board_usb_loop_*`)
 * to the modelled USBFS device the device-side firmware is serving.
 *
 * ENGAGEMENT: the model stays dormant (every access falls through to the
 * sparse register fallback, preserving existing app behaviour byte-for-byte)
 * until BOTH hold:
 *   1. main.c granted the loop (::board_usb_host_set_allowed -- no C-level
 *      usb-host symbol seam was installed for this firmware, so the register
 *      path is the real one), and
 *   2. the firmware selects host mode on the HS instance (a SYSCFG write with
 *      DCFM set -- HUM Ch 37.2.1).
 * Engagement also latches the self-loop (::board_usb_loop_latch), parking the
 * built-in virtual host for the rest of the run.
 *
 * Modelling boundary: single-packet-in-flight, polled transfers only -- what
 * the synchronous `ra8_usb_host_*` engine issues. A multi-chunk control-OUT
 * data stage overwrites the device's single DCP staging bank if the device
 * firmware does not drain between chunks (no consumer in the EIL suite does
 * this; DFU class flows stay on their C-level seams).
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
 * @brief Reset the USBHS host-mode model to its power-on state.
 *
 * @details Clears the register shadows, the per-pipe configuration window,
 * the FIFO staging and the engagement latch. Call once from board_periph_init
 * before the run loop, next to ::board_usb_init.
 *
 * @param[in] trace When true, each loop-cable transaction is logged to stderr.
 * @return Nothing.
 * @post The model is dormant; every window access falls through until the
 *       firmware engages host mode (and the loop is allowed).
 * @since 0.1.0
 */
void board_usb_host_init(bool trace);

/**
 * @brief Grant (or deny) the register-level loop for this firmware.
 *
 * @details main.c calls this after its usb-host seam installer ran: when a
 * C-level seam (virtual keyboard / virtual MSC disk) was installed for the
 * firmware's host API, the register path is shadowed by those seams and this
 * model must stay dormant so seamed apps keep their exact current behaviour.
 * Only an unseamed firmware (e.g. the TrustZone NS image, whose symbols the
 * seam installer never scans) gets the register-level loop.
 *
 * @param[in] allowed true to allow engagement on a host-mode SYSCFG write.
 * @return Nothing.
 * @post With @p allowed false the model never claims the window this run.
 * @since 0.1.0
 */
void board_usb_host_set_allowed(bool allowed);

/**
 * @brief Dispatch an MMIO read inside the USBHS register window.
 *
 * @param[in,out] uc      Unicorn engine (loop-cable calls pend device IRQs).
 * @param[in]     addr    Absolute peripheral address being read.
 * @param[in]     size    Access width in bytes (1 / 2 / 4).
 * @param[out]    handled True iff the model is engaged and @p addr is inside
 *                        the USBHS window.
 * @return The register value when @p *handled is true, else 0.
 * @since 0.1.0
 */
uint64_t board_usb_host_read(uc_engine* uc, uint64_t addr, unsigned size, bool* handled);

/**
 * @brief Dispatch an MMIO write inside the USBHS register window.
 *
 * @details Pre-engagement, only the SYSCFG word is watched (for the DCFM
 * host-role select that engages the model); the write still reports
 * unhandled so the sparse fallback keeps recording it, preserving the
 * behaviour of every USBHS app that never engages.
 *
 * @param[in,out] uc      Unicorn engine (loop-cable calls pend device IRQs).
 * @param[in]     addr    Absolute peripheral address being written.
 * @param[in]     size    Access width in bytes (1 / 2 / 4).
 * @param[in]     value   Value being written.
 * @param[out]    handled True iff the model is engaged and consumed the write.
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_host_write(uc_engine* uc,
                          uint64_t   addr,
                          unsigned   size,
                          uint64_t   value,
                          bool*      handled);

/**
 * @brief Print the USBHS host-model section of the end-of-run summary.
 *
 * @details Silent unless the model engaged; then reports the SETUP / bulk
 * transaction totals the firmware host drove over the loop.
 *
 * @return Nothing.
 * @since 0.1.0
 */
void board_usb_host_report(void);

#ifdef __cplusplus
}
#endif
