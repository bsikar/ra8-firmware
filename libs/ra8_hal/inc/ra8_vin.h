/**
 * @file ra8_vin.h
 * @brief Video Input Module (VIN) driver -- public API
 * @ingroup grp_hal_camera
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
 * - lifecycle: `ra8_vin_init` / `ra8_vin_deinit`
 * - capture: `ra8_vin_capture_start` (single / continuous /
 * field-skip / interlaced), `ra8_vin_capture_stop`
 * - pre-clip: `ra8_vin_set_preclip`
 * - scaling: `ra8_vin_set_uds_scale` /
 * `ra8_vin_set_uds_passband` /
 * `ra8_vin_set_uds_clip` /
 * `ra8_vin_set_uds_ctrl`
 * - LUT: `ra8_vin_lut_program` (256 entries Y/Cb/Cr)
 * - colour conv.: `ra8_vin_set_yc_to_rgb` / `ra8_vin_set_rgb_to_yc`
 * - dithering: `ra8_vin_set_dithering`
 * - YUV-444 mode: `ra8_vin_set_yuv444_mode`
 * - DMR options: `ra8_vin_set_data_mode`
 * - CSI-2 input: `ra8_vin_set_csi_input` /
 * `ra8_vin_set_field_detect`
 * - framebuffers: `ra8_vin_set_framebuffers` /
 * `ra8_vin_set_uv_offset` (YC-separated)
 * - status: `ra8_vin_get_status` / `ra8_vin_clear_status` /
 * `ra8_vin_get_module_status` /
 * `ra8_vin_get_line_count` /
 * `ra8_vin_get_active_buffer`
 * - IRQ glue: `ra8_vin_attach_handler` + `ra8_vin_dispatch` +
 * `ra8_vin_set_interrupt_enable` +
 * `ra8_vin_set_scanline_compare`
 * - power: `ra8_vin_enter_stop` / `ra8_vin_exit_stop` /
 * `ra8_vin_reset`
 *
 * The driver enforces NASA Power-of-10 rules: every loop has a fixed
 * upper bound, every public function returns an `ra8_err_t`, and pre/
 * post-condition checks run in both release and unit-test builds.
 *
 * @par Umbrella header
 * This file is a thin umbrella. The driver declarations live in the
 * concern-split sub-headers below and are pulled in verbatim; every
 * consumer that `#include "ra8_vin.h"` continues to see the full API:
 *  - @ref ra8_vin_types.h -- typed enums, configuration descriptor,
 *    register-bundle / colour-space coefficient structs, decoded
 *    status views, and the event / frame-end callback typedefs.
 *  - @ref ra8_vin_api.h -- lifecycle, capture, geometry / scaling,
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

#include "ra8_err.h"
#include "ra8_vin_api.h"
#include "ra8_vin_regs.h"
#include "ra8_vin_types.h"

#ifdef __cplusplus
}
#endif
