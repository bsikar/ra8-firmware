//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Encode and decode a plain Zig struct, the wire layout derived at comptime.
//!
//! Fields are written in declaration order with no padding:
//!
//! * a fixed-width integer is its bytes, little-endian;
//! * an enum is its integer tag, encoded as that integer;
//! * a `[]const u8` is a `u32` length and then that many bytes;
//! * a tagged union is its integer tag and then the active field, which is
//!   any of the above, another tagged union, or `void` for nothing at all.
//!
//! A union's tag must be an enum of explicit width, `union(enum(u8))` or a
//! named enum, so the tag on the wire never depends on how many fields there
//! are.
//!
//! A slice has no size of its own, so its owner bounds each one by name:
//!
//!     const Write = struct {
//!         addr: u32,
//!         data: []const u8,
//!         pub const max_len = .{ .data = 256 };
//!     };
//!
//! That bound is what gives every message a comptime `maxSize`, and it is
//! enforced in both directions. A slice inside a union is bounded by the
//! union's own `max_len`. Any other field type is a compile error.
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
    /// An enum or union field carries a tag its type does not name.
    BadTag,
    /// Input remains after the last field.
    Trailing,
};

/// The integer a slice's length travels as.
pub const Len = u32;

/// How one value travels: as an integer, as an enum's integer tag, as
/// length-prefixed bytes no longer than the payload says, as a tagged union,
/// or, for a `void` union field, as nothing.
const Shape = union(enum) { int: type, tag: type, bytes: usize, choice: type, none };

/// The largest encoding any value of `T` can have.
pub fn maxSize(comptime T: type) comptime_int {
    var total = 0;
    for (fields(T)) |field| total += shapeMax(shape(T, field.name, field.type));
    return total;
}

/// The exact encoding size of `value`, or `Oversize` if a slice is too long.
pub fn size(comptime T: type, value: T) Error!usize {
    var total: usize = 0;
    inline for (comptime fields(T)) |field| {
        const how = comptime shape(T, field.name, field.type);
        total += try shapeSize(how, @field(value, field.name));
    }
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
        put(comptime shape(T, field.name, field.type), @field(value, field.name), out, &at);
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
        const how = comptime shape(T, field.name, field.type);
        @field(value, field.name) = try take(field.type, how, &rest);
    }
    if (rest.len != 0) return error.Trailing;
    return value;
}

fn shapeMax(comptime how: Shape) comptime_int {
    switch (how) {
        .int, .tag => |Int| return @sizeOf(Int),
        .bytes => |max| return @sizeOf(Len) + max,
        .none => return 0,
        .choice => |Union| {
            var widest = 0;
            const info = @typeInfo(Union).@"union";
            for (info.field_names, info.field_types) |name, FieldType| {
                widest = @max(widest, shapeMax(shape(Union, name, FieldType)));
            }
            return @sizeOf(TagInt(Union)) + widest;
        },
    }
}

fn shapeSize(comptime how: Shape, value: anytype) Error!usize {
    switch (how) {
        .int, .tag => |Int| return @sizeOf(Int),
        .none => return 0,
        .bytes => |max| {
            if (value.len > max) return error.Oversize;
            return @sizeOf(Len) + value.len;
        },
        .choice => |Union| switch (value) {
            inline else => |active, tag| {
                const inner = comptime shape(Union, @tagName(tag), @TypeOf(active));
                return @sizeOf(TagInt(Union)) + try shapeSize(inner, active);
            },
        },
    }
}

/// Write one value. The caller has already checked that it fits.
fn put(comptime how: Shape, value: anytype, out: []u8, at: *usize) void {
    switch (how) {
        .int => |Int| putInt(Int, value, out, at),
        .tag => |Int| putInt(Int, @backingInt(value), out, at),
        .none => {},
        .bytes => {
            putInt(Len, @intCast(value.len), out, at);
            @memcpy(out[at.*..][0..value.len], value);
            at.* += value.len;
        },
        .choice => |Union| switch (value) {
            inline else => |active, tag| {
                putInt(TagInt(Union), @backingInt(tag), out, at);
                put(comptime shape(Union, @tagName(tag), @TypeOf(active)), active, out, at);
            },
        },
    }
}

fn take(comptime T: type, comptime how: Shape, rest: *[]const u8) Error!T {
    switch (how) {
        .int => |Int| return takeInt(Int, rest),
        .tag => |Int| return std.meta.intToEnum(T, try takeInt(Int, rest)) catch error.BadTag,
        .none => return {},
        .bytes => |max| return takeBytes(max, rest),
        .choice => {
            const raw = try takeInt(TagInt(T), rest);
            const tag = std.meta.intToEnum(std.meta.Tag(T), raw) catch return error.BadTag;
            switch (tag) {
                inline else => |which| {
                    const name = @tagName(which);
                    const Active = @FieldType(T, name);
                    const active = try take(Active, comptime shape(T, name, Active), rest);
                    return @unionInit(T, name, active);
                },
            }
        },
    }
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

/// One struct field as the codec walks it: 0.17's type info keeps names
/// and types in separate slices.
const Field = struct { name: [:0]const u8, type: type };

fn fields(comptime T: type) []const Field {
    if (@typeInfo(T) != .@"struct") @compileError(@typeName(T) ++ " is not a struct");
    return comptime list: {
        const info = @typeInfo(T).@"struct";
        var out: [info.field_names.len]Field = undefined;
        for (&out, info.field_names, info.field_types) |*f, name, Type| f.* = .{ .name = name, .type = Type };
        const done = out;
        break :list &done;
    };
}

/// How the field `name` of `Owner`, a struct or a union, travels.
fn shape(comptime Owner: type, comptime name: []const u8, comptime T: type) Shape {
    const where = @typeName(Owner) ++ "." ++ name;
    switch (@typeInfo(T)) {
        .int => return .{ .int = wireInt(T) },
        .@"enum" => |info| return .{ .tag = wireInt(info.tag_type) },
        .@"union" => |info| {
            if (info.tag_type == null) @compileError(where ++ ": union has no tag");
            _ = wireInt(TagInt(T));
            return .{ .choice = T };
        },
        .void => if (@typeInfo(Owner) == .@"union") return .none,
        else => if (T == []const u8) return .{ .bytes = bound(Owner, name) },
    }
    @compileError(where ++ ": unsupported field type");
}

/// The integer a union's tag travels as.
fn TagInt(comptime Union: type) type {
    return @typeInfo(std.meta.Tag(Union)).@"enum".tag_type;
}

/// Only the widths whose size is their bit count: no `u24`, no `usize`-shaped
/// surprises that change with the target.
fn wireInt(comptime Int: type) type {
    switch (@typeInfo(Int).int.bits) {
        8, 16, 32, 64 => return Int,
        else => @compileError(@typeName(Int) ++ " is not 8, 16, 32 or 64 bits wide"),
    }
}

fn bound(comptime Owner: type, comptime name: []const u8) usize {
    if (!@hasDecl(Owner, "max_len") or !@hasField(@TypeOf(Owner.max_len), name)) {
        @compileError(@typeName(Owner) ++ "." ++ name ++ ": slice needs a bound in max_len");
    }
    const max: usize = @field(Owner.max_len, name);
    if (max > std.math.maxInt(Len)) {
        @compileError(@typeName(Owner) ++ "." ++ name ++ ": bound too big");
    }
    return max;
}
