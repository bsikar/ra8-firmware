//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the `ra8_box` C ABI membrane: null rejection and the
//! `ra8_err_t` values the published header documents.

const std = @import("std");
const abi = @import("abi");

export fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void {
    _ = tag;
    _ = message;
}

test "error values match the published ra8_err_t codes" {
    try std.testing.expectEqual(@as(u16, 0), @intFromEnum(abi.BoxError.ok));
    try std.testing.expectEqual(@as(u16, 0x103), @intFromEnum(abi.BoxError.invalid_arg));
    try std.testing.expectEqual(@as(u16, 0x504), @intFromEnum(abi.BoxError.null_ptr));
}

test "tree_init rejects null arguments and a zero capacity" {
    var storage: [4]abi.Node = undefined;
    var tree: abi.Tree = undefined;
    try std.testing.expectEqual(abi.BoxError.null_ptr, abi.ra8_box_tree_init(null, &storage, 4));
    try std.testing.expectEqual(abi.BoxError.null_ptr, abi.ra8_box_tree_init(&tree, null, 4));
    try std.testing.expectEqual(abi.BoxError.invalid_arg, abi.ra8_box_tree_init(&tree, &storage, 0));
    try std.testing.expectEqual(abi.BoxError.ok, abi.ra8_box_tree_init(&tree, &storage, 4));
}

test "add and layout reject null arguments" {
    var tree: abi.Tree = undefined;
    const frame = abi.Rect{ .x = 0, .y = 0, .w = 1, .h = 1 };
    try std.testing.expectEqual(@as(i16, -1), abi.ra8_box_add(null, -1, null));
    try std.testing.expectEqual(abi.BoxError.null_ptr, abi.ra8_box_layout(null, 0, &frame));
    try std.testing.expectEqual(abi.BoxError.null_ptr, abi.ra8_box_layout(&tree, 0, null));
}
