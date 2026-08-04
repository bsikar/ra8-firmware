/*
 * SPDX-License-Identifier: MIT
 * Copyright (c) 2026 Brighton Sikarskie
 */
/**
 * @file ra8_dfu_host.h
 * @brief Polled on-board USB-DFU host driver, bound to either controller.
 * @ingroup grp_security
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * The host half of the bidirectional HIL self-loop twins: enumerate the DFU
 * device on the cabled-together jack, DFU_DNLOAD a test image (which the device
 * programs into MRAM), DFU_ABORT back to dfuIDLE, then DFU_UPLOAD it back and
 * byte-compare. Built on the first-party polled `ra8_usb_host_*` /
 * `ra8_usb_host_control_xfer` primitives and parameterized by `ra8_usb_speed_t`,
 * so the same driver runs as the HS host in Config A and the FS host in
 * Config B. Firmware-only (the host test build has no USB controller).
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_err.h"
#include "ra8_usb.h"

/**
 * @struct ra8_dfu_host_result_t
 * @brief Outcome of a DFU round-trip for diagnostics / banners.
 */
typedef struct {
  uint32_t  pid;       /**< Enumerated idProduct.                    */
  uint32_t  blocks_ok; /**< Upload blocks that byte-matched.         */
  uint32_t  mismatch;  /**< First differing block, or 0xFFFFFFFF.    */
  ra8_err_t last_err;  /**< First failing step's error, or k_ra8_ok. */
} ra8_dfu_host_result_t;

/**
 * @brief Enumerate, DFU_DNLOAD @p img, DFU_ABORT, DFU_UPLOAD, and byte-verify.
 *
 * @details Runs the full round-trip on @p host_speed: brings the host
 * controller up, enumerates the DFU device (bus reset -> GET_DESCRIPTOR ->
 * SET_ADDRESS -> SET_CONFIGURATION, into dfuIDLE), downloads @p img one
 * 64-byte block at a time (polling DFU_GETSTATUS to dfuDNLOAD-IDLE between
 * blocks), DFU_ABORTs back to dfuIDLE, then uploads each block and compares to
 * @p img. The device half programs/serves the bytes from real MRAM, so a clean
 * byte-match proves the inactive slot was actually written.
 *
 * @param[in]  host_speed Which controller acts as host (FS or HS).
 * @param[in]  img        Image to download + expected upload bytes (non-NULL).
 * @param[in]  img_len    Image length; non-zero multiple of 64.
 * @param[out] out        Result/diagnostics (non-NULL).
 *
 * @return `ra8_err_t` outcome (also mirrored in `out->last_err`).
 * @retval k_ra8_ok               Download + upload + byte-verify all passed.
 * @retval k_ra8_err_null_ptr     `img` or `out` was NULL.
 * @retval k_ra8_err_invalid_arg  `img_len` is 0 or not a multiple of 64.
 * @retval k_ra8_err_invalid_size An upload block returned the wrong length.
 * @retval k_ra8_err_invalid_state An upload block's bytes differed.
 * @retval k_ra8_err_*            The failing enumerate / transfer code.
 *
 * @pre The two USB jacks are cabled together; `host_speed` is the host jack.
 * @pre `ra8_time_init` has run (ms delays); `img_len % 64 == 0`.
 * @post On any failure the host controller is deinitialized for a clean retry.
 * @post `out` is fully populated.
 * @note Blocking; runs on the host worker thread. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_host_run(ra8_usb_speed_t        host_speed,
                                         const uint8_t*         img,
                                         uint32_t               img_len,
                                         ra8_dfu_host_result_t* out);

/**
 * @brief Enumerate, DFU_DNLOAD @p img, then a zero-length DFU_DNLOAD (manifest).
 *
 * @details The real DFU flash flow -- what `dfu-util` does -- rather than the
 * self-test round-trip of ::ra8_dfu_host_run. Brings the host controller up,
 * enumerates the DFU device, downloads @p img one 64-byte block at a time, then
 * issues a zero-length DFU_DNLOAD that signals end-of-download so the DEVICE
 * commits the slot header (via its worker) and enters dfuMANIFEST. There is no
 * UPLOAD/ABORT: the caller drives the device half on the SAME chip (self-loop)
 * and confirms the commit with ::ra8_dfu_device_committed + ::ra8_dfu_slot_valid.
 *
 * @param[in]  host_speed Which controller acts as host (FS or HS).
 * @param[in]  img        Bootable slot image to flash (non-NULL).
 * @param[in]  img_len    Image length; non-zero multiple of 64.
 * @param[out] out        Result/diagnostics (non-NULL; `pid` populated).
 *
 * @return `ra8_err_t` outcome (also mirrored in `out->last_err`).
 * @retval k_ra8_ok               Image downloaded and the manifest accepted.
 * @retval k_ra8_err_null_ptr     `img` or `out` was NULL.
 * @retval k_ra8_err_invalid_arg  `img_len` is 0 or not a multiple of 64.
 * @retval k_ra8_err_*            The failing enumerate / transfer code.
 *
 * @pre The two USB jacks are cabled together; `host_speed` is the host jack.
 * @pre `ra8_time_init` has run; the device half targets the slot to commit.
 * @post On failure the host controller is deinitialized for a clean retry.
 * @post The device has been told to commit; the caller polls for completion.
 * @note Blocking; runs on the host worker thread. @since 0.1.0
 */
[[nodiscard]] ra8_err_t ra8_dfu_host_program(ra8_usb_speed_t        host_speed,
                                             const uint8_t*         img,
                                             uint32_t               img_len,
                                             ra8_dfu_host_result_t* out);

#ifdef __cplusplus
}
#endif
