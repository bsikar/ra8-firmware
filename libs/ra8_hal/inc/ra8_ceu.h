/**
 * @file ra8_ceu.h
 * @brief Capture Engine Unit (CEU) camera-capture driver
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full-coverage HAL driver for the RA8D2 Capture Engine Unit. The
 * CEU is the parallel camera-input path that DMAs frames from an
 * external CMOS sensor (the EK-RA8D2's on-board OV5640) into
 * SRAM/SDRAM. This driver implements every operating mode and
 * register field documented in HUM Ch 60 (p 3626-3682):
 *
 *  - **Image-capture mode** (CAMCR.JPG = 00) -- YCbCr 4:2:2 input,
 *    optional hardware conversion to YCbCr 4:2:0, optional bilinear
 *    scale-down, optional input-low-pass pre-filter.
 *  - **Data-synchronous-fetch mode** (CAMCR.JPG = 01) -- raw
 *    sensor pixels at HD/VD timing, no Y/C separation.
 *  - **Data-enable-fetch mode** (CAMCR.JPG = 10) -- gated capture
 *    of byte-stream data (e.g. JPEG payloads); CFWCR firewall is
 *    armed automatically here.
 *  - **Single-shot vs continuous capture** (CAPCR.CTNCP).
 *  - **Bundle write mode** (CDOCR.CBE) with 4-address ping-pong
 *    across CDAYR / CDAYR2 / CDBYR / CDBYR2.
 *  - **Interlace** capture with both-field or one-field clipping
 *    (CAIFR.IFS / CIM, CAMCR.FLDPOL/FLDSEL, CDBYR/CDBCR programming).
 *  - **Plane B** shadow programming for in-flight reconfiguration
 *    across a VD edge plus the forced-swap (CRCMPR.RA) escape hatch.
 *  - **Byte swap** at 8/16/32-bit granularity (CDOCR.COBS / COWS /
 *    COLS).
 *  - **TrustZone-style firewall** (CFWCR) capping the legal write
 *    window during data-enable fetch.
 *  - **Frame-drop** counter (CAPCR.FDRP) and **burst-mode** size
 *    (CAPCR.MTCM, 32/64/128/256-byte transfers).
 *  - **Edge select** for the data lines, HD, VD, FLD inputs
 *    (CAMCR.DSEL / HDSEL / VDSEL / FLDSEL).
 *  - **Every IRQ source** documented in CETCR -- frame end, field
 *    end, illegal register write, HD, VD, four bundle-end events,
 *    CRAM overflow, HD/VD mismatch, VD error, firewall fault, HD
 *    missing, VD missing -- with a registered callback dispatch.
 *  - **Lifecycle**: init / deinit / reset / enter_stop / exit_stop
 *    plus a dedicated software-reset path bounded by a CSTSR.CPTON
 *    spin with `k_ra8_err_hw_timeout` on overrun.
 *  - **Status**: CSTSR snapshot (CPTON, CPFLD field, CRST plane),
 *    CDSSR byte-count read after data-enable capture.
 *
 * ## Driver state machine
 *
 * @par State Machine:
 * @dot
 * digraph ra8_ceu_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Closed [label="Closed"];
 *   Idle [label="Idle"];
 *   Capturing [label="Capturing"];
 *   Stopped [label="Stopped"];
 *
 *   __start -> Closed;
 *   Closed -> Idle [label="ra8_ceu_init()"];
 *   Idle -> Capturing [label="ra8_ceu_capture_start()"];
 *   Capturing -> Idle [label="CETCR.CPE (single-shot)"];
 *   Capturing -> Capturing [label="CETCR.CPE (continuous,\nCTNCP=1)"];
 *   Capturing -> Idle [label="ra8_ceu_capture_stop() /\nreset()"];
 *   Idle -> Stopped [label="ra8_ceu_enter_stop()"];
 *   Stopped -> Idle [label="ra8_ceu_exit_stop()"];
 *   Idle -> Closed [label="ra8_ceu_deinit()"];
 * }
 * @enddot
 *
 * | From      | Trigger                | To        |
 * |-----------|------------------------|-----------|
 * | Closed    | `ra8_ceu_init`          | Idle      |
 * | Idle      | `ra8_ceu_capture_start` | Capturing |
 * | Capturing | CPE (single-shot)      | Idle      |
 * | Capturing | `ra8_ceu_capture_stop`  | Idle      |
 * | Capturing | `ra8_ceu_reset`         | Idle      |
 * | Idle      | `ra8_ceu_enter_stop`    | Stopped   |
 * | Stopped   | `ra8_ceu_exit_stop`     | Idle      |
 * | Idle      | `ra8_ceu_deinit`        | Closed    |
 *
 * The driver does not maintain explicit state -- the state is
 * derived from CSTSR.CPTON, CAPSR.CE / CPKIL and the CEIER mask.
 * Every public API checks the live register state before issuing a
 * mode change so the table above is observable, not enforced via a
 * private state variable.
 *
 * @par Umbrella header
 * This file is a thin umbrella. The driver declarations live in the
 * concern-split sub-headers below and are pulled in verbatim; every
 * consumer that `#include "ra8_ceu.h"` continues to see the full API:
 *  - @ref ra8_ceu_types.h -- configuration descriptors, enums,
 *    register-bundle structs, status snapshot, event-callback typedef.
 *  - @ref ra8_ceu_api.h -- lifecycle, status/IRQ, power, capture,
 *    live-reconfig setters, and DMA-coupling function prototypes.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

#include "ra8_ceu_api.h"
#include "ra8_ceu_regs.h"
#include "ra8_ceu_types.h"
#include "ra8_err.h"

#ifdef __cplusplus
}
#endif
