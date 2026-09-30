//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The init-time bump arena (layer 0 of the #147 memory hierarchy, paired with
//! the slab). One contiguous region of a single tier is handed out as aligned
//! sub-blocks by bumping a cursor. There is no free: bring-up carves every
//! fixed buffer once and then never allocates again (NASA Power-of-10 rule 3).
//!
//! Alignment is a property of the ADDRESS, not of the offset into the region,
//! so every fit check works on `usize` addresses taken from the base pointer
//! rather than on the `u32` cursor. The padding an alignment costs is charged
//! to the arena, which is what lets `carveRemaining` report a usable extent
//! instead of a raw remainder.

const std = @import("std");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// `ra8_arena_limits_t`.
pub const Limits = struct {
    /// `k_ra8_arena_slot_cap`: largest slot count `carveAll` accepts.
    pub const slot_cap: u32 = 16;
};

/// `ra8_arena_t`, field for field.
pub const Arena = extern struct {
    base: ?[*]u8 = null,
    size: u32 = 0,
    used: u32 = 0,
    high_water: u32 = 0,

    fn cursor(self: *const Arena) usize {
        return @intFromPtr(self.base) + self.used;
    }

    fn end(self: *const Arena) usize {
        return @intFromPtr(self.base) + self.size;
    }

    fn noteHighWater(self: *Arena) void {
        if (self.used > self.high_water) self.high_water = self.used;
    }
};

/// One named sub-block of a multi-slot workspace carve: `ra8_arena_slot_t`.
///
/// `alignment` is the field C spells `align`, which is a keyword here. The
/// layout is positional and asserted below, so the rename cannot drift.
pub const Slot = extern struct {
    bytes: u32,
    alignment: u32,
    out_ptr: ?*?*anyopaque,
};

comptime {
    std.debug.assert(@offsetOf(Slot, "bytes") == 0);
    std.debug.assert(@offsetOf(Slot, "alignment") == 4);
    std.debug.assert(@offsetOf(Arena, "size") == @sizeOf(usize));
    std.debug.assert(Limits.slot_cap == 16);
}

fn isPow2(v: u32) bool {
    return v != 0 and (v & (v - 1)) == 0;
}

/// Round `addr` up to `alignment`, or null when the rounding leaves the
/// address space. The C formed this sum in `uintptr_t` and relied on it not
/// overflowing; saying so explicitly costs one branch and removes the UB.
fn alignUp(addr: usize, alignment: u32) ?usize {
    const mask: usize = @as(usize, alignment) - 1;
    const sum = @addWithOverflow(addr, mask);
    if (sum[1] != 0) return null;
    return sum[0] & ~mask;
}

pub fn init(arena: *Arena, base: [*]u8, size: u32) Err {
    if (size == 0) return .invalid_size;
    arena.base = base;
    arena.size = size;
    arena.used = 0;
    arena.high_water = 0;
    return .ok;
}

pub fn carve(arena: *Arena, bytes: u32, alignment: u32, out_ptr: *?*anyopaque) Err {
    if (bytes == 0) return .invalid_size;
    if (!isPow2(alignment)) return .invalid_arg;

    const aligned = alignUp(arena.cursor(), alignment) orelse return .no_mem;
    const limit = arena.end();
    if (aligned > limit) return .no_mem;
    if (bytes > limit - aligned) return .no_mem;

    out_ptr.* = @ptrFromInt(aligned);
    arena.used = @intCast(aligned + bytes - @intFromPtr(arena.base));
    arena.noteHighWater();
    return .ok;
}

pub fn remaining(arena: *const Arena) u32 {
    return arena.size - arena.used;
}

pub fn highWater(arena: *const Arena) u32 {
    return arena.high_water;
}

/// Rewind to empty. The peak survives, which is the whole point: a reusable
/// scratch arena reports what a run actually needed.
pub fn reset(arena: *Arena) void {
    arena.used = 0;
}

fn checkSlot(slot: Slot) Err {
    if (slot.out_ptr == null) return .null_ptr;
    if (slot.bytes == 0) return .invalid_size;
    if (!isPow2(slot.alignment)) return .invalid_arg;
    return .ok;
}

/// Prove the whole table fits by carving from a throw-away copy. This is what
/// makes `carveAll` atomic: the caller's arena only moves once every slot has
/// already succeeded here, so a workspace is never half-published.
fn probe(arena: Arena, slots: []const Slot) Err {
    var scratch_arena = arena;
    for (slots) |slot| {
        var scratch: ?*anyopaque = null;
        const err = carve(&scratch_arena, slot.bytes, slot.alignment, &scratch);
        if (err != .ok) return err;
    }
    return .ok;
}

pub fn carveAll(arena: *Arena, slots: []const Slot) Err {
    if (slots.len == 0 or slots.len > Limits.slot_cap) return .invalid_arg;

    for (slots) |slot| {
        const err = checkSlot(slot);
        if (err != .ok) return err;
    }

    const fits = probe(arena.*, slots);
    if (fits != .ok) return fits;

    for (slots) |slot| {
        // The probe already proved every one of these succeeds.
        std.debug.assert(carve(arena, slot.bytes, slot.alignment, slot.out_ptr.?) == .ok);
    }
    return .ok;
}

/// Carve everything left as one aligned block. An empty tail is `no_mem`, not
/// a zero-length block, so a caller is never handed a span it must not write.
pub fn carveRemaining(
    arena: *Arena,
    alignment: u32,
    out_ptr: *?*anyopaque,
    out_bytes: *u32,
) Err {
    if (!isPow2(alignment)) return .invalid_arg;

    const aligned = alignUp(arena.cursor(), alignment) orelse return .no_mem;
    const limit = arena.end();
    if (aligned >= limit) return .no_mem;

    const bytes: u32 = @intCast(limit - aligned);
    const err = carve(arena, bytes, alignment, out_ptr);
    if (err != .ok) return err;
    out_bytes.* = bytes;
    return .ok;
}
