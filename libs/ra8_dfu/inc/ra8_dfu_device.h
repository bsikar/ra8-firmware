/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dfu_device.h
 * @brief USBX DFU device class wired to real MRAM, bound to either controller.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * Front-half of the controller-agnostic DFU device. Registers the vendored
 * USBX DFU class and points its `write`/`read`/`get_status`/`notify` callbacks
 * at the `ra8_dfu_program` MRAM path so a DFU_DNLOAD actually programs the
 * inactive application slot and a DFU_UPLOAD reads it back. Which USB
 * controller it runs on is purely the `ra8_usb_speed_t` passed to
 * ::ra8_dfu_device_start (threaded through the `ux_dcd_ra8_usb` bridge) -- the
 * same core drives the FS device in Config A and the HS device in Config B.
 *
 * The slow MRAM erase/program runs in the caller's device-worker thread via
 * ::ra8_dfu_device_worker_step (the code-MRAM program loop must not run in an
 * ISR or from MRAM); `dfu_write` only stages a block and the host's
 * DFU_GETSTATUS sees dfuDNBUSY until the worker catches up.
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_dfu.h"
#include "ra8_err.h"
#include "ra8_usb.h"

/**
 * @brief Select the slot DFU_DNLOAD programs into / DFU_UPLOAD reads from.
 *
 * @details Records the INACTIVE slot all subsequent DFU transfers act on; an
 * out-of-range value is ignored so the default (Slot B) stands. Set once during
 * init, before ::ra8_dfu_device_start.
 *
 * @param[in] target_slot Slot A or B (must NOT be the slot the caller runs from).
 *
 * @pre Called before ::ra8_dfu_device_start. @pre `target_slot` is A or B.
 * @post Subsequent downloads target that slot. @post Defaults to Slot B if unset.
 * @note Not thread-safe; configure during init. @since 0.1.0
 */
void ra8_dfu_device_set_target(ra8_dfu_slot_t target_slot);

/**
 * @brief Bring up USBX + the DFU class on one controller and raise D+.
 *
 * @details Initialises the USBX system + device stack with the caller's
 * descriptor framework, registers the DFU class with the MRAM-backed
 * callbacks, runs ::ux_dcd_ra8_usb_initialize for `speed`, and attaches the
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
 * @return `ra8_err_t` outcome.
 * @retval k_ra8_ok              Device stack up, class registered, D+ raised.
 * @retval k_ra8_err_null_ptr    A required pointer was NULL.
 * @retval k_ra8_err_invalid_state USBX init / class register failed.
 * @retval k_ra8_err_hw_error    DCD init or attach failed.
 *
 * @pre `usbx_pool`/`framework`/`strings`/`langids` non-NULL; thread context.
 * @pre ::ra8_dfu_device_set_target chose a slot not in use by the caller.
 * @post On success the device is enumerable in dfuIDLE on `speed`.
 * @note Not thread-safe; single init. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_device_start(ra8_usb_speed_t speed,
                                             void*           usbx_pool,
                                             uint32_t        pool_bytes,
                                             unsigned char*  framework,
                                             uint32_t        framework_len,
                                             unsigned char*  strings,
                                             uint32_t        strings_len,
                                             unsigned char*  langids,
                                             uint32_t        langids_len);

/**
 * @brief Program any pending DNLOAD block into MRAM; commit on end-of-download.
 *
 * @details Call repeatedly from the device-worker thread. On the first block it
 * prepares the target slot (::ra8_dfu_program_prepare). Each staged block is
 * programmed via ::ra8_dfu_program_image. When the host signals end-of-download
 * (zero-length DNLOAD / manifest) the slot header is committed
 * (::ra8_dfu_program_commit) so the slot becomes bootable. Slow + SRAM-resident,
 * which is why it lives in thread context and not the DFU callback.
 *
 * @return The most recent program error.
 * @retval k_ra8_ok              Idle, or the pending block programmed cleanly.
 * @retval k_ra8_err_*           The failing ::ra8_dfu_program_* code (latched).
 *
 * @pre ::ra8_dfu_device_start has run. @pre Caller is the device-worker thread.
 * @post A pending block is programmed and the BUSY status is cleared.
 * @post On end-of-download the target slot's header is committed.
 * @note Not thread-safe vs a second worker. @since 0.1.0
 */
ra8_err_t ra8_dfu_device_worker_step(void);

/**
 * @brief Total image bytes accepted so far (diagnostic).
 *
 * @details Returns the value of the internal `img_len` field, which tracks
 * the highest byte offset written: `(block_number * wTransferSize) + padded
 * block length`. Updated by ::ra8_dfu_device_worker_step on each successful
 * DNLOAD block. Intended for J-Link live-watch or host diagnostics; not
 * needed for correct DFU operation.
 *
 * @return Highest end-offset reached across all programmed DNLOAD blocks, in
 *         bytes. Zero if no block has been successfully programmed yet.
 * @retval 0 No blocks have been successfully programmed yet.
 *
 * @pre ::ra8_dfu_device_start has been called and returned ::k_ra8_ok.
 * @pre The caller reads this value only for diagnostic purposes; no lock is
 *      required if single-reader, single-writer (worker thread updates it).
 * @post The returned value is a snapshot; it may increase on the next worker
 *       step if a new block is being programmed concurrently.
 * @post The internal counter is not reset by this call.
 *
 * @note Safe to read from any context; the underlying field is volatile.
 * @since 0.1.0
 */
uint32_t ra8_dfu_device_image_len(void);

/**
 * @brief Count of DNLOAD blocks programmed into MRAM (diagnostic).
 *
 * @details Returns the value of the internal `writes` field, which is
 * incremented once per successfully programmed DNLOAD block inside
 * ::internal_dfu_write. A block is only counted if the MRAM program call
 * returned ::k_ra8_ok; blocks that fault increment `prog_err` instead.
 * Intended for J-Link live-watch or host diagnostics.
 *
 * @return Number of DNLOAD blocks successfully written to MRAM since
 *         ::ra8_dfu_device_start was called.
 * @retval 0 No blocks have been successfully programmed yet.
 *
 * @pre ::ra8_dfu_device_start has been called and returned ::k_ra8_ok.
 * @pre The caller reads this value only for diagnostic purposes; the
 *      underlying field is updated by ::internal_dfu_write (USBX context).
 * @post The returned value is a snapshot; it may increase as further blocks
 *       are received from the host.
 * @post The internal counter is not reset or modified by this call.
 *
 * @note Safe to read from any context; the underlying field is volatile.
 * @since 0.1.0
 */
uint32_t ra8_dfu_device_block_writes(void);

/**
 * @brief Whether the host has signalled end-of-download / manifest.
 *
 * @details Returns the internal `manifest` flag, which is set to `true` by
 * ::internal_dfu_write when a zero-length DNLOAD block (end-of-download) is
 * received, or by ::internal_dfu_notify on a
 * UX_SLAVE_CLASS_DFU_NOTIFICATION_END_DOWNLOAD event. Once set it is never
 * cleared within a session. ::ra8_dfu_device_worker_step polls this flag to
 * decide when to commit the image header.
 *
 * @return `true` once a manifest or end-of-download signal has been received
 *         from the host; `false` while the download is still in progress.
 * @retval true  End-of-download has been signalled; header commit is pending
 *               or already done.
 * @retval false No end-of-download has arrived yet; download still in flight.
 *
 * @pre ::ra8_dfu_device_start has been called and returned ::k_ra8_ok.
 * @pre At least one DFU_DNLOAD transaction has been processed by the USBX
 *      stack (otherwise the flag remains false by initialisation).
 * @post The returned value reflects the `manifest` field at the time of the
 *       call; it will never revert to `false` once it becomes `true`.
 * @post The `manifest` field itself is not modified by this call.
 *
 * @note Safe to read from any context; the underlying field is volatile.
 * @since 0.1.0
 */
bool ra8_dfu_device_manifested(void);

/**
 * @brief Latched last program error (diagnostic).
 *
 * @details Returns the value of `s_dev.prog_err`, which is latched to the
 * first non-::k_ra8_ok result from any ::ra8_dfu_program_image or
 * ::ra8_dfu_program_commit call. Once set to a non-OK code it is never
 * cleared within the session. ::internal_dfu_get_status and
 * ::internal_dfu_write both read this field to decide which USBX
 * media-status code to report to the host.
 *
 * @return The latched program error code.
 * @retval k_ra8_ok        No program fault has occurred in this session.
 * @retval k_ra8_err_*     The error from the first failing ::ra8_dfu_program_*
 *                        call; subsequent errors do not overwrite this value.
 *
 * @pre ::ra8_dfu_device_start has been called and returned ::k_ra8_ok.
 * @pre The field is initialised to ::k_ra8_ok by static zero-initialisation of
 *      the file-scope context; no explicit reset call is needed.
 * @post The returned value is a snapshot of the latch at the time of the call.
 * @post The latch is not reset or modified by this call.
 *
 * @note Safe to read from any context; the underlying field is volatile.
 * @since 0.1.0
 */
ra8_err_t ra8_dfu_device_last_error(void);

/**
 * @brief Whether the image header has been committed (slot now bootable).
 *
 * @details Returns the internal `committed` flag, which is set to `true` by
 * ::ra8_dfu_device_worker_step immediately after ::ra8_dfu_program_commit
 * succeeds (or even on failure, to prevent repeated commit attempts).
 * Goes true after ::ra8_dfu_device_worker_step writes the header on
 * end-of-download. A bootloader polls this to know when it is safe to reset
 * into the freshly programmed slot.
 *
 * @return `true` once the target slot's header has been written; `false`
 *         while the download is still in progress or the commit has not yet
 *         run.
 * @retval true  The slot header has been committed; the slot is now bootable
 *               (assuming no program error was latched).
 * @retval false The commit has not yet run; download may still be in flight.
 *
 * @pre ::ra8_dfu_device_start has been called and returned ::k_ra8_ok.
 * @pre ::ra8_dfu_device_manifested returns `true` before this flag becomes
 *      `true`; commit follows manifest in the worker step.
 * @post The returned value reflects the `committed` field at the time of the
 *       call; it will never revert to `false` once it becomes `true`.
 * @post The `committed` field itself is not modified by this call.
 *
 * @note Safe to read from any context; the underlying field is volatile.
 * @since 0.1.0
 */
bool ra8_dfu_device_committed(void);

#ifdef __cplusplus
}
#endif
