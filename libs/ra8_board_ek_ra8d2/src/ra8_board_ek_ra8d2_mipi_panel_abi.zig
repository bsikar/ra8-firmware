//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI of the J32 MIPI panel bring-up: `ra8_board_mipi_dsi_init`, as
//! `inc/ra8_board_ek_ra8d2_peripherals.h` declares it. Kept apart from
//! `ra8_board_ek_ra8d2_abi.zig` only to keep both files short.

const mipi_panel = @import("internal/mipi_panel.zig");

export fn ra8_board_mipi_dsi_init() u32 {
    return mipi_panel.init();
}
