/**
 * @file ra_dfu_device.h
 * @brief USBX DFU device class wired to real MRAM, bound to either controller.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Front-half of the controller-agnostic DFU device. Registers the vendored
 * USBX DFU class and points its `write`/`read`/`get_status`/`notify` callbacks
 * at the `ra_dfu_program` MRAM path so a DFU_DNLOAD actually programs the
 * inactive application slot and a DFU_UPLOAD reads it back. Which USB
 * controller it runs on is purely the `ra_usb_speed_t` passed to
 * ::ra_dfu_device_start (threaded through the `ux_dcd_ra_usb` bridge) -- the
 * same core drives the FS device in Config A and the HS device in Config B.
 *
 * The slow MRAM erase/program runs in the caller's device-worker thread via
 * ::ra_dfu_device_worker_step (the code-MRAM program loop must not run in an
 * ISR or from MRAM); `dfu_write` only stages a block and the host's
 * DFU_GETSTATUS sees dfuDNBUSY until the worker catches up.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra_dfu.h"
#include "ra_err.h"
#include "ra_usb.h"

/**
 * @brief Select the slot DFU_DNLOAD programs into / DFU_UPLOAD reads from.
 *
 * @param[in] target_slot Slot A or B (must NOT be the slot the caller runs from).
 *
 * @pre Called before ::ra_dfu_device_start. @pre `target_slot` is A or B.
 * @post Subsequent downloads target that slot. @post Defaults to Slot B if unset.
 * @note Not thread-safe; configure during init. @since 0.1.0
 */
void ra_dfu_device_set_target(ra_dfu_slot_t target_slot);

/**
 * @brief Bring up USBX + the DFU class on one controller and raise D+.
 *
 * @details Initialises the USBX system + device stack with the caller's
 * descriptor framework, registers the DFU class with the MRAM-backed
 * callbacks, runs ::ux_dcd_ra_usb_initialize for `speed`, and attaches the
 * D+ pull-up. The device enumerates directly into dfuIDLE (DFU-mode interface).
 *
 * @param[in] speed         Which USB controller (FS or HS).
 * @param[in] usbx_pool     Static byte pool for USBX (non-NULL).
 * @param[in] pool_bytes    Size of `usbx_pool`.
 * @param[in] framework     USB descriptor framework (device + config + DFU).
 * @param[in] framework_len Length of `framework`.
 * @param[in] strings       String-descriptor framework.
 * @param[in] strings_len   Length of `strings`.
 * @param[in] langids       LANGID framework.
 * @param[in] langids_len   Length of `langids`.
 *
 * @return `ra_err_t` outcome.
 * @retval k_ra_ok              Device stack up, class registered, D+ raised.
 * @retval k_ra_err_null_ptr    A required pointer was NULL.
 * @retval k_ra_err_invalid_state USBX init / class register failed.
 * @retval k_ra_err_hw_error    DCD init or attach failed.
 *
 * @pre `usbx_pool`/`framework`/`strings`/`langids` non-NULL; thread context.
 * @pre ::ra_dfu_device_set_target chose a slot not in use by the caller.
 * @post On success the device is enumerable in dfuIDLE on `speed`.
 * @note Not thread-safe; single init. @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dfu_device_start(ra_usb_speed_t speed,
                                           void*          usbx_pool,
                                           uint32_t       pool_bytes,
                                           unsigned char* framework,
                                           uint32_t       framework_len,
                                           unsigned char* strings,
                                           uint32_t       strings_len,
                                           unsigned char* langids,
                                           uint32_t       langids_len);

/**
 * @brief Program any pending DNLOAD block into MRAM; commit on end-of-download.
 *
 * @details Call repeatedly from the device-worker thread. On the first block it
 * prepares the target slot (::ra_dfu_program_prepare). Each staged block is
 * programmed via ::ra_dfu_program_image. When the host signals end-of-download
 * (zero-length DNLOAD / manifest) the slot header is committed
 * (::ra_dfu_program_commit) so the slot becomes bootable. Slow + SRAM-resident,
 * which is why it lives in thread context and not the DFU callback.
 *
 * @return The most recent program error.
 * @retval k_ra_ok              Idle, or the pending block programmed cleanly.
 * @retval k_ra_err_*           The failing ::ra_dfu_program_* code (latched).
 *
 * @pre ::ra_dfu_device_start has run. @pre Caller is the device-worker thread.
 * @post A pending block is programmed and the BUSY status is cleared.
 * @post On end-of-download the target slot's header is committed.
 * @note Not thread-safe vs a second worker. @since 0.1.0
 */
ra_err_t ra_dfu_device_worker_step(void);

/**
 * @brief Total image bytes accepted so far (diagnostic).
 * @return Highest `(block + 1) * wTransferSize` seen. @pre None. @post None.
 * @note JLink-readable. @since 0.1.0
 */
uint32_t ra_dfu_device_image_len(void);

/**
 * @brief Count of DNLOAD blocks programmed into MRAM (diagnostic).
 * @return Programmed-block counter. @pre None. @post None.
 * @note JLink-readable. @since 0.1.0
 */
uint32_t ra_dfu_device_block_writes(void);

/**
 * @brief Whether the host has signalled end-of-download / manifest.
 * @return true once a manifest/end-of-download notification arrived.
 * @pre None. @post None. @note JLink-readable. @since 0.1.0
 */
bool ra_dfu_device_manifested(void);

/**
 * @brief Latched last program error (diagnostic).
 * @return k_ra_ok when healthy, else the failing program code.
 * @pre None. @post None. @note JLink-readable. @since 0.1.0
 */
ra_err_t ra_dfu_device_last_error(void);

#ifdef __cplusplus
}
#endif
