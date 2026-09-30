//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The error codes and limits are pinned against the values `ra8_err.h` and
//! `ra8_slab.h` publish, so a change on either side of the membrane fails here
//! rather than at a call site.

const std = @import("std");
const vocab = @import("vocab");

test "error codes match ra8_err.h" {
    try std.testing.expectEqual(@as(u16, 0), vocab.Err.ok.code());
    try std.testing.expectEqual(@as(u16, 0x102), vocab.Err.no_mem.code());
    try std.testing.expectEqual(@as(u16, 0x103), vocab.Err.invalid_arg.code());
    try std.testing.expectEqual(@as(u16, 0x104), vocab.Err.invalid_state.code());
    try std.testing.expectEqual(@as(u16, 0x105), vocab.Err.invalid_size.code());
    try std.testing.expectEqual(@as(u16, 0x504), vocab.Err.null_ptr.code());
}

test "published limits match the headers" {
    try std.testing.expectEqual(@as(u32, 0xFFFF_FFFF), vocab.SlabLimits.nil);
    try std.testing.expectEqual(@as(u32, 4), vocab.SlabLimits.min_cell_bytes);
    try std.testing.expectEqual(@as(u32, 4), vocab.SlabLimits.align_bytes);
}
