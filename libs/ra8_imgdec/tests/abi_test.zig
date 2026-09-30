//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The published record layouts. The comptime asserts in `abi.zig` do the real
//! pinning; these read them back at runtime so a layout drift shows up as a
//! failing test rather than only as a build error.

const std = @import("std");
const vocab = @import("vocab");

const ptr_bytes = @sizeOf(usize);

test "the capability record is five words and a flag" {
    try std.testing.expectEqual(@as(usize, 24), @sizeOf(vocab.Caps));
    try std.testing.expectEqual(@as(usize, 20), @offsetOf(vocab.Caps, "streams"));
}

test "the request keeps its three pointers where c puts them" {
    try std.testing.expectEqual(ptr_bytes, @offsetOf(vocab.Req, "byte_count"));
    try std.testing.expectEqual(2 * ptr_bytes, @offsetOf(vocab.Req, "arena"));
    try std.testing.expectEqual(3 * ptr_bytes, @offsetOf(vocab.Req, "dst"));
}

test "the image record ends with the alpha flag" {
    try std.testing.expectEqual(@as(usize, 28), @sizeOf(vocab.Image));
    try std.testing.expectEqual(@as(usize, 24), @offsetOf(vocab.Image, "had_alpha"));
}

test "the geometry record is three words" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(vocab.Geom));
}

test "a handle is a vtable pointer and a context pointer" {
    try std.testing.expectEqual(2 * ptr_bytes, @sizeOf(vocab.Handle));
    try std.testing.expectEqual(ptr_bytes, @offsetOf(vocab.Handle, "ctx"));
}

test "the name record pads its enum up to pointer alignment" {
    try std.testing.expectEqual(ptr_bytes, @offsetOf(vocab.Name, "ext"));
    try std.testing.expectEqual(3 * ptr_bytes, @sizeOf(vocab.Name));
}

test "the scratch record is five machine words" {
    try std.testing.expectEqual(5 * ptr_bytes, @sizeOf(vocab.Scratch));
}

test "the mux holds four members before its count" {
    try std.testing.expectEqual(@as(u32, 4), vocab.mux_max);
    try std.testing.expectEqual(vocab.mux_max * 2 * ptr_bytes, @offsetOf(vocab.Mux, "count"));
}

test "a zeroed handle and a zeroed mux are the unbound states" {
    const dec = vocab.Handle{};
    try std.testing.expect(dec.iface == null);
    const set = vocab.Mux{};
    try std.testing.expectEqual(@as(u32, 0), set.count);
}
