//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the production `ra8_devcfg_store_t` binding over the extra-MRAM
//! window. These run against the host RAM shadow, which is what the C's
//! `RA8_OFF_TARGET` build exercised; the silicon path is proven by the ARM
//! cross-build and the page-loop arithmetic tested directly below.
//!
//! The shadow is process-lifetime state with no reset seam, exactly as the C
//! file had it, so each case owns a disjoint offset range and the blank-read
//! case runs first.

const std = @import("std");
const store = @import("store");

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

export fn ra8_log_emit_warn(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

export fn ra8_log_emit_error_val(tag: [*:0]const u8, message: [*:0]const u8, value: u32) void {
    _ = tag;
    _ = message;
    _ = value;
}

const ok: u16 = 0;
const err_out_of_range: u16 = 0x208;
const err_null_ptr: u16 = 0x504;

/// The seams as the header declares them: `uint8_t*`, which C may pass NULL
/// through. `ra8_devcfg_store_t` spells them non-optional on the Zig side, so
/// the null-argument cases below go through these views of the same pointers.
const ReadNullable = *const fn (u32, ?[*]u8, u32) callconv(.c) u16;
const WriteNullable = *const fn (u32, ?[*]const u8, u32) callconv(.c) u16;

fn backends() struct { read: ReadNullable, write: WriteNullable } {
    const s = store.ra8_devcfg_default_store();
    return .{ .read = @ptrCast(s.read.?), .write = @ptrCast(s.write.?) };
}

test "default store exposes both seams" {
    const s = store.ra8_devcfg_default_store();
    try std.testing.expect(s.read != null);
    try std.testing.expect(s.write != null);
}

test "default store is process-lifetime state, not a fresh vtable per call" {
    try std.testing.expectEqual(store.ra8_devcfg_default_store(), store.ra8_devcfg_default_store());
}

test "a never-programmed window reads back blank" {
    const s = backends();
    var got: [16]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(0x00, &got, got.len));
    for (got) |byte| try std.testing.expectEqual(store.blank, byte);
}

test "blank is what fails the record magic, so a virgin unit resolves unprovisioned" {
    try std.testing.expectEqual(@as(u8, 0xFF), store.blank);
}

test "a programmed byte range reads back through copy 0" {
    const s = backends();
    const payload = [_]u8{ 0xDE, 0xAD, 0xBE, 0xEF, 0x01, 0x02, 0x03, 0x04 };
    try std.testing.expectEqual(ok, s.write(store.copy0_off, &payload, payload.len));
    var got: [payload.len]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(store.copy0_off, &got, got.len));
    try std.testing.expectEqualSlices(u8, &payload, &got);
}

test "copy 1 is a distinct slot: programming it leaves copy 0 alone" {
    const s = backends();
    const first = [_]u8{ 0x11, 0x22, 0x33, 0x44 };
    const second = [_]u8{ 0x55, 0x66, 0x77, 0x88 };
    try std.testing.expectEqual(ok, s.write(store.copy0_off, &first, first.len));
    try std.testing.expectEqual(ok, s.write(store.copy1_off, &second, second.len));
    var got: [4]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(store.copy0_off, &got, got.len));
    try std.testing.expectEqualSlices(u8, &first, &got);
    try std.testing.expectEqual(ok, s.read(store.copy1_off, &got, got.len));
    try std.testing.expectEqualSlices(u8, &second, &got);
}

test "a write longer than one program page round-trips whole" {
    const s = backends();
    var payload: [128]u8 = undefined;
    for (&payload, 0..) |*byte, i| byte.* = @truncate(i *% 7 +% 3);
    try std.testing.expectEqual(ok, s.write(store.copy1_off, &payload, payload.len));
    var got: [128]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(store.copy1_off, &got, got.len));
    try std.testing.expectEqualSlices(u8, &payload, &got);
}

test "a read that starts past the region is refused" {
    const s = backends();
    var got: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_out_of_range, s.read(store.span, &got, got.len));
}

test "a write that starts past the region is refused" {
    const s = backends();
    const payload: [8]u8 = @splat(0xA5);
    try std.testing.expectEqual(err_out_of_range, s.write(store.span, &payload, payload.len));
}

test "a read that runs off the end by one byte is refused" {
    const s = backends();
    var got: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_out_of_range, s.read(store.span - 7, &got, got.len));
}

test "a read that ends exactly on the region end is allowed" {
    const s = backends();
    var got: [8]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(store.span - 8, &got, got.len));
}

test "a null destination is refused before the bounds check" {
    const s = backends();
    try std.testing.expectEqual(err_null_ptr, s.read(store.span, null, 8));
}

test "a null source is refused before the bounds check" {
    const s = backends();
    try std.testing.expectEqual(err_null_ptr, s.write(store.span, null, 8));
}

test "a zero-length access at the region end is not an overrun" {
    const s = backends();
    var got: [1]u8 = @splat(0);
    try std.testing.expectEqual(ok, s.read(store.span, &got, 0));
}

test "a length near the 32-bit ceiling is refused rather than wrapping past the guard" {
    const s = backends();
    var got: [8]u8 = @splat(0);
    try std.testing.expectEqual(err_out_of_range, s.read(0x100, &got, 0xFFFFFF00));
}

test "the region spans copy 1 plus one reserved slot" {
    try std.testing.expectEqual(store.copy1_off + store.slot_bytes, store.span);
    try std.testing.expectEqual(@as(u32, 448), store.span);
}

test "the schema constants are the ones the C header publishes" {
    try std.testing.expectEqual(@as(u32, 0x40), store.copy0_off);
    try std.testing.expectEqual(@as(u32, 0x100), store.copy1_off);
    try std.testing.expectEqual(@as(u32, 192), store.slot_bytes);
    try std.testing.expectEqual(@as(u32, 32), store.page_bytes);
    try std.testing.expectEqual(@as(u32, 0x02E07600), store.flash_extra_start);
}

test "inRegion accepts what fits and rejects what does not" {
    try std.testing.expect(store.inRegion(0, store.span));
    try std.testing.expect(store.inRegion(store.span, 0));
    try std.testing.expect(!store.inRegion(0, store.span + 1));
    try std.testing.expect(!store.inRegion(store.span, 1));
    try std.testing.expect(!store.inRegion(0xFFFFFFFF, 1));
}

test "a program chunk never crosses a page" {
    try std.testing.expectEqual(store.page_bytes, store.chunkBytes(128, 0));
    try std.testing.expectEqual(store.page_bytes, store.chunkBytes(128, 32));
    try std.testing.expectEqual(store.page_bytes, store.chunkBytes(128, 96));
    try std.testing.expectEqual(@as(u32, 5), store.chunkBytes(37, 32));
    try std.testing.expectEqual(@as(u32, 1), store.chunkBytes(1, 0));
}

test "the page loop covers a whole record in four chunks and overshoots none" {
    const record_len: u32 = 128;
    var done: u32 = 0;
    var chunks: u32 = 0;
    while (done < record_len) : (chunks += 1) {
        const chunk = store.chunkBytes(record_len, done);
        try std.testing.expect(chunk > 0 and chunk <= store.page_bytes);
        done += chunk;
    }
    try std.testing.expectEqual(record_len, done);
    try std.testing.expectEqual(@as(u32, 4), chunks);
}

test "the page loop terminates on a length that is not a page multiple" {
    const len: u32 = 100;
    var done: u32 = 0;
    var chunks: u32 = 0;
    while (done < len) : (chunks += 1) {
        done += store.chunkBytes(len, done);
    }
    try std.testing.expectEqual(len, done);
    try std.testing.expectEqual(@as(u32, 4), chunks);
}

test "the host build backs the store with the RAM shadow" {
    try std.testing.expect(store.off_target);
}
