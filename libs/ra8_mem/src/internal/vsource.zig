//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The object-source registry (Layer 1, #147): the map from an `object_id` to
//! the backing that holds its bytes, and the loader the Layer-2 page cache
//! calls on a miss.
//!
//! Two kinds of backing share one registry. A **paged** object reads through a
//! callback the app binds to a block device or a file, so this layer names no
//! storage dependency at all. An **XIP** object is already memory-mapped, so it
//! is handed back as a pointer with no copy and no cache frame.
//!
//! The C spelled the distinction as `xip != NULL` and re-tested it at each use.
//! Here it is resolved once into `Backing`, so a paged object with no callback
//! is an error code rather than a call through a null pointer, and a frame
//! arrives as a slice of its real length, so every copy is bounds-checked
//! rather than trusting `&o->xip[offset]`.
//!
//! Zero allocation (NASA P10 Rule 3): the caller owns the object array.

const std = @import("std");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_vsource_read_fn`: read `len` bytes at `offset` from a paged object's
/// backing. Stored in the object, so it costs the archive no extern.
pub const ReadFn = *const fn (
    ctx: ?*anyopaque,
    offset: u64,
    buf: [*]u8,
    len: u32,
) callconv(.c) u16;

/// `ra8_vsource_obj_t`, field for field. The caller allocates the array, so
/// the layout is C's.
pub const Obj = extern struct {
    read: ?ReadFn = null,
    ctx: ?*anyopaque = null,
    xip: ?[*]const u8 = null,
    base: u64 = 0,
    size: u64 = 0,
};

/// `ra8_vsource_t`, field for field, with `count <= cap` as its invariant.
pub const Registry = extern struct {
    objs: ?[*]Obj = null,
    cap: u32 = 0,
    count: u32 = 0,
};

/// What an object's bytes actually come from, resolved from `Obj` once.
const Backing = union(enum) {
    xip: [*]const u8,
    paged: struct { read: ReadFn, ctx: ?*anyopaque, base: u64 },
};

fn backingOf(obj: *const Obj) ?Backing {
    if (obj.xip) |base| return .{ .xip = base };
    const read = obj.read orelse return null;
    return .{ .paged = .{ .read = read, .ctx = obj.ctx, .base = obj.base } };
}

/// Bind an empty registry over a caller-owned object array.
pub fn init(self: *Registry, objs: []Obj) Err {
    if (objs.len == 0) return .invalid_size;
    self.* = .{ .objs = objs.ptr, .cap = @intCast(objs.len), .count = 0 };
    return .ok;
}

/// The registered objects, as a slice. Empty until `init` binds the array.
fn slots(self: *const Registry) []Obj {
    const objs = self.objs orelse return &.{};
    return objs[0..self.count];
}

fn object(self: *const Registry, object_id: u32) ?*const Obj {
    const registered = slots(self);
    if (object_id >= registered.len) return null;
    return &registered[object_id];
}

fn append(self: *Registry, obj: Obj, out_id: *u32) Err {
    const objs = self.objs orelse return .invalid_state;
    if (obj.size == 0) return .invalid_size;
    if (self.count >= self.cap) return .no_mem;
    objs[self.count] = obj;
    out_id.* = self.count;
    self.count += 1;
    return .ok;
}

/// Register a storage-paged object; `out_id` receives its `object_id`.
pub fn addPaged(
    self: *Registry,
    read: ReadFn,
    ctx: ?*anyopaque,
    base: u64,
    size: u64,
    out_id: *u32,
) Err {
    return append(self, .{ .read = read, .ctx = ctx, .base = base, .size = size }, out_id);
}

/// Register a memory-mapped object; `out_id` receives its `object_id`.
pub fn addXip(self: *Registry, xip: [*]const u8, size: u64, out_id: *u32) Err {
    return append(self, .{ .xip = xip, .size = size }, out_id);
}

/// Fill a page frame from an object: the `ra8_vmem_loader_fn` adapter. The
/// whole frame is zeroed first, so a short tail at the object's end reads back
/// as zeros.
pub fn load(self: *const Registry, object_id: u32, offset: u64, frame: []u8) Err {
    const obj = object(self, object_id) orelse return .out_of_range;
    if (offset >= obj.size) return .out_of_range;

    const wanted = @min(obj.size - offset, @as(u64, frame.len));
    const to_fill = std.math.cast(usize, wanted) orelse return .out_of_range;
    @memset(frame, 0);

    switch (backingOf(obj) orelse return .invalid_state) {
        .xip => |base| {
            const start = std.math.cast(usize, offset) orelse return .out_of_range;
            @memcpy(frame[0..to_fill], base[start..][0..to_fill]);
            return .ok;
        },
        .paged => |p| {
            return Err.from(p.read(p.ctx, p.base + offset, frame.ptr, @intCast(to_fill)));
        },
    }
}

/// A direct pointer into an XIP object: no copy, no cache frame.
pub fn xipPtr(self: *const Registry, object_id: u32, offset: u64, len: u32) union(enum) {
    ptr: [*]const u8,
    failed: Err,
} {
    const obj = object(self, object_id) orelse return .{ .failed = .out_of_range };
    const base = obj.xip orelse return .{ .failed = .not_supported };
    if (offset >= obj.size) return .{ .failed = .out_of_range };
    if (len > obj.size - offset) return .{ .failed = .out_of_range };
    const start = std.math.cast(usize, offset) orelse return .{ .failed = .out_of_range };
    return .{ .ptr = base + start };
}

comptime {
    // The caller allocates both of these (a `static` in the shelf example, a
    // stack local in the suites), so their widths are C's, not Zig's choice.
    // Spelled out per pointer width rather than derived from the struct under
    // test, which would assert nothing.
    const ptr_bytes = @sizeOf(usize);
    std.debug.assert(@offsetOf(Obj, "read") == 0);
    std.debug.assert(@offsetOf(Obj, "ctx") == ptr_bytes);
    std.debug.assert(@offsetOf(Obj, "xip") == 2 * ptr_bytes);
    std.debug.assert(@offsetOf(Obj, "base") == if (ptr_bytes == 8) 24 else 16);
    std.debug.assert(@offsetOf(Obj, "size") == if (ptr_bytes == 8) 32 else 24);
    std.debug.assert(@sizeOf(Obj) == if (ptr_bytes == 8) 40 else 32);

    std.debug.assert(@offsetOf(Registry, "objs") == 0);
    std.debug.assert(@offsetOf(Registry, "cap") == ptr_bytes);
    std.debug.assert(@offsetOf(Registry, "count") == ptr_bytes + 4);
    std.debug.assert(@sizeOf(Registry) == if (ptr_bytes == 8) 16 else 12);
}
