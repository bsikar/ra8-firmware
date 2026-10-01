//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the C membrane: the null guards, the `ra8_err_t` codes, and the
//! out-parameters the C callers read back.

const std = @import("std");
const abi = @import("abi");
const rpc_stub = @import("mdl_rpc_stub");

comptime {
    // the media RPC seam the coordinator calls into; C owns it in the archive
    _ = rpc_stub;
}

const ok: u16 = 0;
const invalid_size: u16 = 0x105;
const null_ptr: u16 = 0x504;
const overhead: u16 = 12;

test "open reports the body offset and succeeds" {
    var buf: [64]u8 = undefined;
    var body_at: u16 = 0xFFFF;

    try std.testing.expectEqual(ok, abi.priv_c6link_tlv_open(&buf, buf.len, 4, &body_at));
    try std.testing.expectEqual(overhead, body_at);
}

test "open rejects null out and null body_at" {
    var buf: [64]u8 = undefined;
    var body_at: u16 = 7;

    try std.testing.expectEqual(null_ptr, abi.priv_c6link_tlv_open(null, 64, 4, &body_at));
    try std.testing.expectEqual(null_ptr, abi.priv_c6link_tlv_open(&buf, buf.len, 4, null));
}

test "open zeroes body_at before the capacity check" {
    var buf: [64]u8 = undefined;
    var body_at: u16 = 0xFFFF;

    try std.testing.expectEqual(invalid_size, abi.priv_c6link_tlv_open(&buf, 4, 4, &body_at));
    try std.testing.expectEqual(@as(u16, 0), body_at);
}

test "open honours the declared capacity, not the real buffer" {
    var buf: [64]u8 = undefined;
    var body_at: u16 = 0;

    try std.testing.expectEqual(invalid_size, abi.priv_c6link_tlv_open(&buf, overhead + 3, 4, &body_at));
    try std.testing.expectEqual(ok, abi.priv_c6link_tlv_open(&buf, overhead + 4, 4, &body_at));
}

test "body returns a pointer into the payload and the declared length" {
    var buf: [64]u8 = undefined;
    var body_at: u16 = 0;
    try std.testing.expectEqual(ok, abi.priv_c6link_tlv_open(&buf, buf.len, 3, &body_at));
    @memset(buf[body_at..][0..3], 0x55);

    var proto_len: u16 = 0xFFFF;
    const found = abi.priv_c6link_tlv_body(&buf, body_at + 3, &proto_len).?;
    try std.testing.expectEqual(@as(u16, 3), proto_len);
    try std.testing.expectEqual(&buf[overhead], &found[0]);
}

test "body rejects null payload and null proto_len" {
    var buf: [64]u8 = undefined;
    var proto_len: u16 = 0;

    try std.testing.expectEqual(@as(?[*]const u8, null), abi.priv_c6link_tlv_body(null, 16, &proto_len));
    try std.testing.expectEqual(@as(?[*]const u8, null), abi.priv_c6link_tlv_body(&buf, 16, null));
}

test "body zeroes proto_len on a malformed envelope" {
    var buf: [64]u8 = .{0} ** 64;
    var proto_len: u16 = 0xFFFF;

    try std.testing.expectEqual(@as(?[*]const u8, null), abi.priv_c6link_tlv_body(&buf, 32, &proto_len));
    try std.testing.expectEqual(@as(u16, 0), proto_len);
}
