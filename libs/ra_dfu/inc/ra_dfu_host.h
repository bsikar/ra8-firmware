/**
 * @file ra_dfu_host.h
 * @brief Polled on-board USB-DFU host driver, bound to either controller.
 *
 * @par Tag
 * [Ring 4 / Service] {World: S}
 *
 * @details
 * The host half of the bidirectional HIL self-loop twins: enumerate the DFU
 * device on the cabled-together jack, DFU_DNLOAD a test image (which the device
 * programs into MRAM), DFU_ABORT back to dfuIDLE, then DFU_UPLOAD it back and
 * byte-compare. Built on the first-party polled `ra_usb_host_*` /
 * `ra_usb_host_control_xfer` primitives and parameterized by `ra_usb_speed_t`,
 * so the same driver runs as the HS host in Config A and the FS host in
 * Config B. Firmware-only (the host test build has no USB controller).
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
#include "ra_usb.h"

/**
 * @struct ra_dfu_host_result_t
 * @brief Outcome of a DFU round-trip for diagnostics / banners.
 */
typedef struct {
  uint32_t pid;       /**< Enumerated idProduct.                       */
  uint32_t blocks_ok; /**< Upload blocks that byte-matched.            */
  uint32_t mismatch;  /**< First differing block, or 0xFFFFFFFF.       */
  ra_err_t last_err;  /**< First failing step's error, or k_ra_ok.     */
} ra_dfu_host_result_t;

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
 * @return `ra_err_t` outcome (also mirrored in `out->last_err`).
 * @retval k_ra_ok               Download + upload + byte-verify all passed.
 * @retval k_ra_err_null_ptr     `img` or `out` was NULL.
 * @retval k_ra_err_invalid_arg  `img_len` is 0 or not a multiple of 64.
 * @retval k_ra_err_invalid_size An upload block returned the wrong length.
 * @retval k_ra_err_invalid_state An upload block's bytes differed.
 * @retval k_ra_err_*            The failing enumerate / transfer code.
 *
 * @pre The two USB jacks are cabled together; `host_speed` is the host jack.
 * @pre `ra_time_init` has run (ms delays); `img_len % 64 == 0`.
 * @post On any failure the host controller is deinitialized for a clean retry.
 * @post `out` is fully populated.
 * @note Blocking; runs on the host worker thread. @since 0.1.0
 */
[[nodiscard]] ra_err_t ra_dfu_host_run(ra_usb_speed_t        host_speed,
                                       const uint8_t*        img,
                                       uint32_t              img_len,
                                       ra_dfu_host_result_t* out);

#ifdef __cplusplus
}
#endif
