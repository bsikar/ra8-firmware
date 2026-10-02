//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `vocab.Err` must keep the exact `ra8_err_t` numbering from
//! `libs/ra8_core/inc/ra8_err.h`: callers compare these against the C enum.

const std = @import("std");
const vocab = @import("vocab");

test "error codes match ra8_err.h" {
    try std.testing.expectEqual(@as(u16, 0), vocab.Err.ok.code());
    try std.testing.expectEqual(@as(u16, 0x103), vocab.Err.invalid_arg.code());
    try std.testing.expectEqual(@as(u16, 0x104), vocab.Err.invalid_state.code());
    try std.testing.expectEqual(@as(u16, 0x105), vocab.Err.invalid_size.code());
    try std.testing.expectEqual(@as(u16, 0x106), vocab.Err.not_found.code());
    try std.testing.expectEqual(@as(u16, 0x107), vocab.Err.not_supported.code());
    try std.testing.expectEqual(@as(u16, 0x10A), vocab.Err.no_data.code());
    try std.testing.expectEqual(@as(u16, 0x504), vocab.Err.null_ptr.code());
}

test "the enum crosses the ABI as uint16_t" {
    try std.testing.expectEqual(@as(usize, 2), @sizeOf(vocab.Err));
}

test "success is the only zero value" {
    try std.testing.expectEqual(@as(u16, 0), vocab.Err.ok.code());
    inline for (@typeInfo(vocab.Err).@"enum".fields) |field| {
        if (!std.mem.eql(u8, field.name, "ok")) {
            try std.testing.expect(field.value != 0);
        }
    }
}
