//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! `ra8_imgdec_scratch_t`: the bump allocator a backend carves its working set
//! from, over storage the caller owns.
//!
//! Nothing here reaches an arena. `carve` is the one entry that needs one, and
//! it takes the block already carved rather than the arena itself, which is
//! what keeps this file free of the ring below it and testable on the host.

const abi = @import("abi.zig");
const vocab = @import("vocab.zig");

const Scratch = abi.Scratch;
const Err = vocab.Err;

pub const Limits = struct {
    /// Alignment every block is rounded up to.
    pub const alignment: u32 = 16;
};

/// `bytes` rounded up to `Limits.alignment`, or null when that would overflow.
fn roundUp(bytes: usize) ?usize {
    const slack: usize = Limits.alignment - 1;
    if (bytes > std_max_usize - slack) return null;
    return (bytes + slack) & ~slack;
}

const std_max_usize: usize = ~@as(usize, 0);

fn usable(scratch: *const Scratch) bool {
    return scratch.base != null and scratch.cap != 0;
}

/// Take `bytes` off the cursor, or null when the remaining capacity cannot
/// cover the rounded-up request.
fn reserve(scratch: *Scratch, bytes: usize) ?[*]u8 {
    const want = roundUp(bytes) orelse return null;
    if (want > scratch.cap - scratch.offset) return null;

    const block = scratch.base.? + scratch.offset;
    scratch.offset += want;
    scratch.live += 1;
    if (scratch.offset > scratch.high_water) scratch.high_water = scratch.offset;
    return block;
}

fn isPow2(value: u32) bool {
    return (value & (value -% 1)) == 0;
}

/// Point `scratch` at `store`.
pub fn init(scratch: *Scratch, store: []u8) u16 {
    if (store.len == 0) return Err.invalid_size;
    scratch.* = .{ .base = store.ptr, .cap = store.len };
    return Err.ok;
}

/// Drop every outstanding block without touching the backing store.
pub fn reset(scratch: *Scratch) void {
    scratch.offset = 0;
    scratch.live = 0;
}

pub fn alloc(scratch: *Scratch, bytes: usize) ?[*]u8 {
    if (!usable(scratch) or bytes == 0) return null;
    return reserve(scratch, bytes);
}

pub fn calloc(scratch: *Scratch, count: usize, size: usize) ?[*]u8 {
    if (!usable(scratch) or count == 0 or size == 0) return null;
    if (count > std_max_usize / size) return null;

    const bytes = count * size;
    const block = reserve(scratch, bytes) orelse return null;
    @memset(block[0..bytes], 0);
    return block;
}

pub fn realloc(scratch: *Scratch, ptr: ?[*]u8, old_bytes: usize, new_bytes: usize) ?[*]u8 {
    if (!usable(scratch) or new_bytes == 0) return null;

    const block = reserve(scratch, new_bytes) orelse return null;
    if (ptr) |old| {
        const carry = @min(old_bytes, new_bytes);
        if (carry != 0) @memcpy(block[0..carry], old[0..carry]);
        free(scratch, old);
    }
    return block;
}

/// Release one block. The cursor only rewinds once the last one is gone, which
/// is what makes this a bump allocator rather than a heap.
pub fn free(scratch: *Scratch, ptr: ?[*]u8) void {
    if (ptr == null or scratch.live == 0) return;
    scratch.live -= 1;
    if (scratch.live == 0) scratch.offset = 0;
}

pub fn highWater(scratch: *const Scratch) usize {
    return scratch.high_water;
}

/// The alignment a carve of `align_req` actually uses, or an error code when
/// the request cannot be honoured.
///
/// The arena call itself sits at the membrane: this answers the part of
/// `ra8_imgdec_scratch_carve` that is a decision rather than an allocation.
pub fn carveAlign(bytes: u32, align_req: u32) union(enum) { ok: u32, fault: u16 } {
    if (bytes == 0) return .{ .fault = Err.invalid_size };

    const want = if (align_req == 0) Limits.alignment else align_req;
    if (!isPow2(want)) return .{ .fault = Err.invalid_arg };
    if (want > Limits.alignment) return .{ .fault = Err.not_supported };
    return .{ .ok = want };
}
