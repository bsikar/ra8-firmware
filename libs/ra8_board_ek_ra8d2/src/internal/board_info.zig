//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Board identity, from the EK-RA8D2 v1 User's Manual cover page
//! (R20UT5523EG0101 Rev 1.01, October 2025).

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

/// The C `ra8_board_info_t` this layer fills.
pub const Info = hal.BoardInfo;

pub const name: [:0]const u8 = "EK-RA8D2 v1";
pub const doc_rev: [:0]const u8 = "R20UT5523EG0101 Rev 1.01";
pub const mcu: [:0]const u8 = "R7KA8D2KFLCAC";

pub fn fill(out: *Info) u32 {
    out.* = .{ .name = name.ptr, .doc_rev = doc_rev.ptr, .mcu = mcu.ptr };
    return vocab.Err.ok;
}
