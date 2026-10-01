//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Module root for the Zig half of the EK-RA8D2 board layer. Pulls the ABI
//! surface in so the static archive carries it; the internal modules are
//! reached through there, not from here.

pub const abi = @import("ra8_board_ek_ra8d2_abi.zig");

comptime {
    _ = abi;
}
