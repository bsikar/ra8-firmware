//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_board_ra8p1`: board-support layer for the Renesas RA8P1
//! (R7KA8P1KFLCAC), in Zig. Translates board coordinates ("LED1", "SW1", "the
//! console") into HAL calls.
//!
//! The C ABI the firmware links against lives in `ra8_board_ra8p1_abi.zig`;
//! referencing it here is what pulls those exports into the archive.

pub const abi = @import("ra8_board_ra8p1_abi.zig");
pub const console = @import("internal/console.zig");
pub const identity = @import("internal/identity.zig");
pub const pins = @import("internal/pins.zig");
pub const vocab = @import("internal/vocab.zig");

comptime {
    _ = abi;
}
