//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Encode and decode a plain Zig struct, the wire layout derived at comptime.
//!
//! Fields are written in declaration order with no padding:
//!
//! * a fixed-width integer is its bytes, little-endian;
//! * an enum is its integer tag, encoded as that integer;
//! * a `[]const u8` is a `u32` length and then that many bytes.
//!
//! A slice has no size of its own, so a message bounds each one by name:
//!
//!     const Write = struct {
//!         addr: u32,
//!         data: []const u8,
//!         pub const max_len = .{ .data = 256 };
//!     };
//!
//! That bound is what gives every message a comptime `maxSize`, and it is
//! enforced in both directions. Any other field type is a compile error.
//!
//! The encoding is canonical: one value has one encoding, and a buffer that
//! decodes at all re-encodes to the same bytes.

const std = @import("std");

pub const Error = error{
    /// The output buffer is smaller than the encoding.
    NoSpace,
    /// The input ended before the value did.
    Truncated,
    /// A slice is longer than its message allows.
    Oversize,
    /// An enum field carries a tag its type does not name.
    BadTag,
    /// Input remains after the last field.
    Trailing,
};

/// The integer a slice's length travels as.
pub const Len = u32;

const Field = std.builtin.Type.StructField;

/// How one field travels: as an integer, as an enum's integer tag, or as
/// length-prefixed bytes no longer than the payload says.
const Shape = union(enum) { int: type, tag: type, bytes: usize };

/// The largest encoding any value of `T` can have.
pub fn maxSize(comptime T: type) comptime_int {
    var total = 0;
    for (fields(T)) |field| total += switch (shape(T, field)) {
        .int, .tag => |Int| @sizeOf(Int),
        .bytes => |max| @sizeOf(Len) + max,
    };
    return total;
}

/// The exact encoding size of `value`, or `Oversize` if a slice is too long.
pub fn size(comptime T: type, value: T) Error!usize {
    var total: usize = 0;
    inline for (comptime fields(T)) |field| switch (comptime shape(T, field)) {
        .int, .tag => |Int| total += @sizeOf(Int),
        .bytes => |max| {
            const bytes = @field(value, field.name);
            if (bytes.len > max) return error.Oversize;
            total += @sizeOf(Len) + bytes.len;
        },
    };
    return total;
}

/// Encode `value` at the front of `out` and return the bytes written.
///
/// `out` is untouched on error. A slice in `value` must not overlap `out`.
pub fn encode(comptime T: type, value: T, out: []u8) Error![]u8 {
    const need = try size(T, value);
    if (out.len < need) return error.NoSpace;

    var at: usize = 0;
    inline for (comptime fields(T)) |field| {
        const item = @field(value, field.name);
        switch (comptime shape(T, field)) {
            .int => |Int| putInt(Int, item, out, &at),
            .tag => |Int| putInt(Int, @intFromEnum(item), out, &at),
            .bytes => {
                putInt(Len, @intCast(item.len), out, &at);
                @memcpy(out[at..][0..item.len], item);
                at += item.len;
            },
        }
    }
    return out[0..at];
}

/// Decode exactly one `T` from the whole of `in`.
///
/// Slices in the result point into `in`; nothing is copied.
pub fn decode(comptime T: type, in: []const u8) Error!T {
    var value: T = undefined;
    var rest = in;
    inline for (comptime fields(T)) |field| {
        @field(value, field.name) = switch (comptime shape(T, field)) {
            .int => |Int| try takeInt(Int, &rest),
            .tag => |Int| std.meta.intToEnum(field.type, try takeInt(Int, &rest)) catch
                return error.BadTag,
            .bytes => |max| try takeBytes(max, &rest),
        };
    }
    if (rest.len != 0) return error.Trailing;
    return value;
}

fn putInt(comptime Int: type, value: Int, out: []u8, at: *usize) void {
    std.mem.writeInt(Int, out[at.*..][0..@sizeOf(Int)], value, .little);
    at.* += @sizeOf(Int);
}

fn takeInt(comptime Int: type, rest: *[]const u8) Error!Int {
    if (rest.len < @sizeOf(Int)) return error.Truncated;
    defer rest.* = rest.*[@sizeOf(Int)..];
    return std.mem.readInt(Int, rest.*[0..@sizeOf(Int)], .little);
}

/// The bound is checked before the remaining input, and neither check adds to
/// the claimed length, so no length can wrap its way past them.
fn takeBytes(max: usize, rest: *[]const u8) Error![]const u8 {
    const len = try takeInt(Len, rest);
    if (len > max) return error.Oversize;
    if (len > rest.len) return error.Truncated;
    defer rest.* = rest.*[len..];
    return rest.*[0..len];
}

fn fields(comptime T: type) []const Field {
    if (@typeInfo(T) != .@"struct") @compileError(@typeName(T) ++ " is not a struct");
    return std.meta.fields(T);
}

fn shape(comptime T: type, comptime field: Field) Shape {
    return switch (@typeInfo(field.type)) {
        .int => .{ .int = wireInt(field.type) },
        .@"enum" => |info| .{ .tag = wireInt(info.tag_type) },
        else => if (field.type == []const u8)
            .{ .bytes = bound(T, field.name) }
        else
            @compileError(@typeName(T) ++ "." ++ field.name ++ ": unsupported field type"),
    };
}

/// Only the widths whose size is their bit count: no `u24`, no `usize`-shaped
/// surprises that change with the target.
fn wireInt(comptime Int: type) type {
    switch (@typeInfo(Int).int.bits) {
        8, 16, 32, 64 => return Int,
        else => @compileError(@typeName(Int) ++ " is not 8, 16, 32 or 64 bits wide"),
    }
}

fn bound(comptime T: type, comptime name: []const u8) usize {
    if (!@hasDecl(T, "max_len") or !@hasField(@TypeOf(T.max_len), name)) {
        @compileError(@typeName(T) ++ "." ++ name ++ ": slice needs a bound in max_len");
    }
    const max: usize = @field(T.max_len, name);
    if (max > std.math.maxInt(Len)) @compileError(@typeName(T) ++ "." ++ name ++ ": bound too big");
    return max;
}
