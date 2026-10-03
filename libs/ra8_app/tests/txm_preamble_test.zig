//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Tests for the ThreadX module preamble reader, on a synthetic module laid
//! out the way upstream's `txm_module_preamble.S` assembles one.

const std = @import("std");
const module_pack = @import("module_pack");
const preamble = module_pack.preamble;

const image_len: usize = 0x2D4;

/// A module image with the preamble words txm_hello_m33 carries.
fn module() [image_len]u8 {
    var bytes = [_]u8{0} ** image_len;
    const words = [_]u32{
        preamble.id, 6, 1,     32,    0x12345678, 0x02000007, 0x211,     0x175,
        0,           1, 0x400, 0x16D, 1,          0x400,      image_len, 0x9C,
    };
    for (words, 0..) |value, index| {
        std.mem.writeInt(u32, bytes[index * 4 ..][0..4], value, .little);
    }
    return bytes;
}

fn put(bytes: []u8, index: usize, value: u32) void {
    std.mem.writeInt(u32, bytes[index * 4 ..][0..4], value, .little);
}

test "the shell entry is measured from its own word with the Thumb bit dropped" {
    const bytes = module();
    const read = try preamble.read(&bytes);
    try std.testing.expectEqual(@as(u32, 0x228), read.entry_offset);
    try std.testing.expectEqual(@as(u32, 0x400), read.stack_size);
    try std.testing.expectEqual(@as(u32, image_len), read.code_size);
    try std.testing.expectEqual(@as(u32, 0x9C), read.ram_data_size);
}

test "an image without the MODU ID is not a module" {
    var bytes = module();
    put(&bytes, preamble.Word.id, 0x4D4F4456);
    try std.testing.expectError(preamble.Error.NotAModule, preamble.read(&bytes));
}

test "an image shorter than the preamble words is refused" {
    const bytes = module();
    try std.testing.expectError(preamble.Error.ShortImage, preamble.read(bytes[0 .. preamble.Word.read_count * 4 - 1]));
}

test "a code size that is not the image length is refused" {
    var bytes = module();
    put(&bytes, preamble.Word.code_size, image_len - 4);
    try std.testing.expectError(preamble.Error.SizeMismatch, preamble.read(&bytes));
}

test "a shell entry past the code is refused" {
    var bytes = module();
    put(&bytes, preamble.Word.shell_entry, image_len);
    try std.testing.expectError(preamble.Error.BadEntry, preamble.read(&bytes));
    put(&bytes, preamble.Word.shell_entry, 0xFFFF_FFFF);
    try std.testing.expectError(preamble.Error.BadEntry, preamble.read(&bytes));
}
