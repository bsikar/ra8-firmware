/**
 * @file ra8_mipi_csi.h
 * @brief MIPI CSI-2 receiver HAL driver -- public API
 * @ingroup grp_hal_camera
 *
 * @par Tag
 * [Ring 3 / HAL] {World: NS}
 *
 * @details
 * Full HUM Ch 66 ("MIPI CSI Interface" p 3935-3972) HAL surface for
 * the RA8D2 MIPI CSI-2 receiver block. Every documented register
 * and every documented IRQ source is exposed.
 *
 * ## Functional groups
 *
 *  - **Lifecycle**          -- init / deinit / reset.
 *  - **Reception control**  -- start / stop receive (MCT3.RXEN).
 *  - **Data-type filter**   -- DTEL / DTEH (per-DT enable).
 *  - **ECC / scrambling**   -- MCT0.ECCV13, MCT0.LFSREN.
 *  - **Frame-error mode**   -- MCT0.ZLMD / EDMD / RVMD.
 *  - **EPD tuning**         -- EPCT (long/short spacers + Option-2).
 *  - **LRTE tuning**        -- EMCT.VLSIEN / EOTPEN.
 *  - **Module status**      -- MCG (RO IP info), MIST (IRQ summary).
 *  - **Receive status**     -- RXST / RXSC / RXIE.
 *  - **Per-data-lane IRQ**  -- DLST(N) / DLSC(N) / DLIE(N).
 *  - **Per-VC IRQ**         -- VCST(M) / VCSC(M) / VCIE(M).
 *  - **Power management**   -- PMST / PMSC / PMIE.
 *  - **Short-packet FIFO**  -- GSCT / GSST / GSSC / GSIE / GSHT / GSIU.
 *  - **ISR dispatch**       -- one dispatcher per HUM IRQ source
 *                              (RX, DL, VC, PM, GST).
 *  - **Power transition**   -- enter_stop / exit_stop (MSTP gate).
 *
 * The driver pairs with the parallel ``ra8_mipi_phy`` driver (HUM
 * Ch 64) which configures the D-PHY itself. ``ra8_mipi_phy`` is
 * owned by a different agent and is NOT invoked from here -- the
 * caller is expected to bring the PHY up before calling
 * ``ra8_mipi_csi_init`` and tear it down after ``ra8_mipi_csi_deinit``.
 *
 * @par State Machine
 * @dot
 * digraph ra8_mipi_csi_states {
 *   bgcolor="transparent";
 *   rankdir=LR;
 *   node [shape=box, style="rounded,filled", fontname="Helvetica", fontsize=10,
 *         fillcolor="#e8eef7", color="#5a7ca6"];
 *   edge [fontname="Helvetica", fontsize=9, color="#5a7ca6"];
 *
 *   __start [shape=circle, width=0.18, label="", fillcolor="#5a7ca6", color="#5a7ca6"];
 *
 *   Gated [label="Gated"];
 *   Idle [label="Idle"];
 *   Active [label="Active"];
 *   Stopped [label="Stopped"];
 *
 *   __start -> Gated [label="reset"];
 *   Gated -> Idle [label="init() [MSTP on, RXEN=0]"];
 *   Idle -> Active [label="start_receive() [RXEN=1]"];
 *   Active -> Idle [label="stop_receive() [RXEN=0,\\nVSRST]"];
 *   Idle -> Gated [label="deinit()"];
 *   Idle -> Stopped [label="enter_stop()"];
 *   Stopped -> Idle [label="exit_stop()"];
 * }
 * @enddot
 *
 * @par Header layout
 * This is a thin umbrella header. The public API is split across the
 * sub-headers below to stay within the per-file line budget; including
 * ``ra8_mipi_csi.h`` pulls in the entire surface unchanged.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include "ra8_mipi_csi_ctrl.h"
#include "ra8_mipi_csi_isr.h"
#include "ra8_mipi_csi_types.h"

#ifdef __cplusplus
}
#endif
