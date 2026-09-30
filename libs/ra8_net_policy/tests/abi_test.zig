//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The published vocabulary: address class values and widths, the error codes
//! the exports return, and the authority capacity callers size buffers to.
//! These are the numbers a C caller compiled against `ra8_net_urlguard.h`
//! already holds, so they are pinned here rather than left to drift.

const std = @import("std");
const testing = std.testing;

const vocab = @import("vocab");

test "the address class is a byte-wide enum with the published values" {
    try testing.expectEqual(@as(usize, 1), @sizeOf(vocab.AddrClass));
    try testing.expectEqual(u8, @typeInfo(vocab.AddrClass).@"enum".tag_type);
    try testing.expectEqual(@as(u8, 0), @intFromEnum(vocab.AddrClass.public));
    try testing.expectEqual(@as(u8, 1), @intFromEnum(vocab.AddrClass.loopback));
    try testing.expectEqual(@as(u8, 2), @intFromEnum(vocab.AddrClass.private));
    try testing.expectEqual(@as(u8, 3), @intFromEnum(vocab.AddrClass.linklocal));
    try testing.expectEqual(@as(u8, 4), @intFromEnum(vocab.AddrClass.unknown));
}

test "the error codes match ra8_err.h" {
    try testing.expectEqual(@as(u16, 0), vocab.err.ok);
    try testing.expectEqual(@as(u16, 0x102), vocab.err.no_mem);
    try testing.expectEqual(@as(u16, 0x103), vocab.err.invalid_arg);
    try testing.expectEqual(@as(u16, 0x106), vocab.err.not_found);
}

test "the published authority capacity is unchanged" {
    try testing.expectEqual(@as(u16, 262), vocab.limits.host_cap);
}

test "the address widths are the wire widths" {
    try testing.expectEqual(@as(usize, 4), vocab.limits.v4_bytes);
    try testing.expectEqual(@as(usize, 16), vocab.limits.v6_bytes);
    try testing.expectEqual(@as(usize, 8), vocab.limits.v6_groups);
    try testing.expectEqual(@as(usize, 12), vocab.limits.v6_mapped_v4);
}
