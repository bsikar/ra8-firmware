/**
 * @file ra_vin.h
 * @brief Video Input Module (VIN) driver -- public API
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * / Round-3 driver for the RA8D2 Video Input Module (VIN).
 * The block sits between the MIPI CSI-2 receiver and external SDRAM
 * and writes captured frames into one of three memory-base buffers
 * (MB1..MB3). It optionally pre-clips, scales (UDS), look-up-table
 * remaps, and colour-space converts the data en-route.
 *
 * The public API mirrors the register surface documented in HUM
 * Ch 67 "Video Input Module" p 3973-4031:
 *
 * - lifecycle: `ra_vin_init` / `ra_vin_deinit`
 * - capture: `ra_vin_capture_start` (single / continuous /
 * field-skip / interlaced), `ra_vin_capture_stop`
 * - pre-clip: `ra_vin_set_preclip`
 * - scaling: `ra_vin_set_uds_scale` /
 * `ra_vin_set_uds_passband` /
 * `ra_vin_set_uds_clip` /
 * `ra_vin_set_uds_ctrl`
 * - LUT: `ra_vin_lut_program` (256 entries Y/Cb/Cr)
 * - colour conv.: `ra_vin_set_yc_to_rgb` / `ra_vin_set_rgb_to_yc`
 * - dithering: `ra_vin_set_dithering`
 * - YUV-444 mode: `ra_vin_set_yuv444_mode`
 * - DMR options: `ra_vin_set_data_mode`
 * - CSI-2 input: `ra_vin_set_csi_input` /
 * `ra_vin_set_field_detect`
 * - framebuffers: `ra_vin_set_framebuffers` /
 * `ra_vin_set_uv_offset` (YC-separated)
 * - status: `ra_vin_get_status` / `ra_vin_clear_status` /
 * `ra_vin_get_module_status` /
 * `ra_vin_get_line_count` /
 * `ra_vin_get_active_buffer`
 * - IRQ glue: `ra_vin_attach_handler` + `ra_vin_dispatch` +
 * `ra_vin_set_interrupt_enable` +
 * `ra_vin_set_scanline_compare`
 * - power: `ra_vin_enter_stop` / `ra_vin_exit_stop` /
 * `ra_vin_reset`
 *
 * The driver enforces NASA Power-of-10 rules: every loop has a fixed
 * upper bound, every public function returns an `ra_err_t`, and pre/
 * post-condition checks run in both release and unit-test builds.
 *
 * @par Umbrella header
 * This file is a thin umbrella. The driver declarations live in the
 * concern-split sub-headers below and are pulled in verbatim; every
 * consumer that `#include "ra_vin.h"` continues to see the full API:
 *  - @ref ra_vin_types.h -- typed enums, configuration descriptor,
 *    register-bundle / colour-space coefficient structs, decoded
 *    status views, and the event / frame-end callback typedefs.
 *  - @ref ra_vin_api.h -- lifecycle, capture, geometry / scaling,
 *    LUT, colour conversion, DMR options, CSI-2 / framebuffers,
 *    status, IRQ path, power, and the dynamic-window / frame-end
 *    function prototypes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8d2_vin_regs.h"
#include "ra_err.h"
#include "ra_vin_api.h"
#include "ra_vin_types.h"

#ifdef __cplusplus
}
#endif
