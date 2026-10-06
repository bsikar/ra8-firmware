//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/wdt_ofs.zig (RA8FW-888).

const std = @import("std");
const ofs = @import("wdt_ofs");

test "erased word decodes to register-start defaults" {
    const d = ofs.decode(0xFFFFFFFF);
    try std.testing.expectEqual(@as(u8, 3), d.timeout);
    try std.testing.expectEqual(@as(u8, 0xF), d.clock_div);
    try std.testing.expectEqual(@as(u8, 3), d.window_start);
    try std.testing.expectEqual(@as(u8, 3), d.window_end);
    try std.testing.expectEqual(@as(u8, 1), d.on_expiry);
    try std.testing.expectEqual(@as(u8, 1), d.stop_in_sleep);
    try std.testing.expectEqual(@as(u8, 1), d.start_mode);
    try std.testing.expect(!d.auto_start);
}

test "each field lands in its own slot" {
    // TOPS=2, CKS=8, RPES=1, RPSS=2, RSTIRQS=1, STPCTL=0, STRT=0.
    const word: u32 = (2 << 18) | (8 << 20) | (1 << 24) | (2 << 26) | (1 << 28);
    const d = ofs.decode(word);
    try std.testing.expectEqual(@as(u8, 2), d.timeout);
    try std.testing.expectEqual(@as(u8, 8), d.clock_div);
    try std.testing.expectEqual(@as(u8, 1), d.window_end);
    try std.testing.expectEqual(@as(u8, 2), d.window_start);
    try std.testing.expectEqual(@as(u8, 1), d.on_expiry);
    try std.testing.expectEqual(@as(u8, 0), d.stop_in_sleep);
    try std.testing.expect(d.auto_start);
}

test "OFS3_SEL rejects mixed multi-bit fields" {
    try std.testing.expect(ofs.selLegal(0));
    try std.testing.expect(ofs.selLegal(0xFFFFFFFF));
    try std.testing.expect(ofs.selLegal(ofs.field_mask));
    try std.testing.expect(!ofs.selLegal(1 << 18)); // TOPS half set
    try std.testing.expect(!ofs.selLegal(0x3 << 20)); // CKS 0b0011
    try std.testing.expect(!ofs.selLegal(1 << 24)); // RPES half set
    try std.testing.expect(!ofs.selLegal(1 << 27)); // RPSS half set
    // Single-bit fields can be either way.
    try std.testing.expect(ofs.selLegal((1 << 17) | (1 << 28)));
}

test "mux picks non-secure where SEL is 1, secure elsewhere" {
    const sel: u32 = 0xF << 20; // CKS from OFS3
    const sec: u32 = 0xAAAAAAAA;
    const nonsec: u32 = 0x55555555;
    const got = ofs.mux(sel, sec, nonsec);
    try std.testing.expectEqual(@as(u32, 0x5 << 20), got & (0xF << 20));
    try std.testing.expectEqual(sec & ~@as(u32, 0xF << 20), got & ~@as(u32, 0xF << 20));
    // Bits outside the field union always come from OFS3_SEC.
    try std.testing.expectEqual(sec & ~ofs.field_mask, ofs.mux(0xFFFFFFFF, sec, nonsec) & ~ofs.field_mask);
}
