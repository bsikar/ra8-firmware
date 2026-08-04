/**
 * @file usb_selftest_device.h
 * @brief Device-side worker entry of the soak self-test
 *
 * @details
 * The USBFS device half (USBX MSC over a read-only FAT16 MRAM volume) runs on
 * its own ThreadX worker; this declares that worker so tx_application_define in
 * main.c can spawn it. Defined in usb_selftest_device.c. ThreadX/USBX only, so
 * the declaration is gated out of a (hypothetical) off-target build.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 *
 * @since 0.1.0
 */

#pragma once

#ifndef RA8_OFF_TARGET
#include "tx_api.h"

/**
 * @brief Device-side worker: bring the FS device MSC stack up, then park.
 *
 * @param[in] arg ThreadX entry argument (unused).
 *
 * @pre tx_application_define created this thread.
 * @pre USB-FS pins + 48 MHz clock are up (main did both).
 * @post The FS device is attached and serviceable (USBX runs the BBB/SCSI
 *       state machine on its own class threads).
 * @since 0.1.0
 */
VOID selftest_device_worker(ULONG arg);
#endif /* !RA8_OFF_TARGET */
