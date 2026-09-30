//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C ABI `ra8_imgdec.h`, `ra8_imgdec_mux.h`, `ra8_imgdec_name.h` and
//! `ra8_imgdec_scratch.h` publish, over the Zig implementation behind it.
//!
//! This file is the only one that speaks in raw pointers and NUL-terminated
//! strings, and the only one that names `ra8_arena_*`. Everything it calls
//! takes slices and returns values.

const abi = @import("internal/abi.zig");
const arena = @import("internal/arena.zig");
const dims_mod = @import("internal/dims.zig");
const fabric = @import("internal/fabric.zig");
const mux_mod = @import("internal/mux.zig");
const name_mod = @import("internal/name.zig");
const scratch_mod = @import("internal/scratch.zig");
const sniff_mod = @import("internal/sniff.zig");
const vocab = @import("internal/vocab.zig");

const Caps = abi.Caps;
const Err = vocab.Err;
const Geom = abi.Geom;
const Handle = abi.Handle;
const Image = abi.Image;
const Mux = abi.Mux;
const Req = abi.Req;
const Scratch = abi.Scratch;

// =============================================================================
// The ring below, still C.
// =============================================================================

extern fn ra8_arena_carve(
    arena_ptr: ?*anyopaque,
    bytes: u32,
    alignment: u32,
    out_ptr: *?*anyopaque,
) u16;
extern fn ra8_arena_remaining(arena_ptr: ?*const anyopaque, out_remaining: *u32) u16;

fn arenaRemaining(arena_ptr: ?*anyopaque, out: *u32) u16 {
    return ra8_arena_remaining(arena_ptr, out);
}

fn arenaCarve(arena_ptr: ?*anyopaque, bytes: u32, alignment: u32, out: *?*anyopaque) u16 {
    return ra8_arena_carve(arena_ptr, bytes, alignment, out);
}

const ops = arena.Ops{ .remaining = arenaRemaining, .carve = arenaCarve };

// =============================================================================
// Vocabulary and the pure header readers
// =============================================================================

export fn ra8_imgdec_pixel_bytes(pixel: u32) callconv(.c) u32 {
    return vocab.pixelBytes(pixel);
}

export fn ra8_imgdec_sniff(bytes: ?[*]const u8, byte_count: u32, out: ?*u32) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    slot.* = vocab.Format.none;
    const raw = bytes orelse return Err.null_ptr;

    slot.* = sniff_mod.sniff(raw[0..byte_count]) catch |fault| {
        slot.* = vocab.Format.none;
        return vocab.faultCode(fault);
    };
    return Err.ok;
}

export fn ra8_imgdec_dims(bytes: ?[*]const u8, byte_count: u32, out: ?*Geom) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    slot.* = .{};
    const raw = bytes orelse return Err.null_ptr;

    slot.* = dims_mod.dims(raw[0..byte_count]) catch |fault| {
        slot.* = .{};
        return vocab.faultCode(fault);
    };
    return Err.ok;
}

export fn ra8_imgdec_name(format: u32, out: ?*abi.Name) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const row = name_mod.name(format) catch |fault| return vocab.faultCode(fault);
    slot.* = name_mod.toAbi(row);
    return Err.ok;
}

export fn ra8_imgdec_identify(
    bytes: ?[*]const u8,
    byte_count: u32,
    out: ?*abi.Name,
) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const raw = bytes orelse return Err.null_ptr;

    const row = name_mod.identify(raw[0..byte_count]) catch |fault| {
        return vocab.faultCode(fault);
    };
    slot.* = name_mod.toAbi(row);
    return Err.ok;
}

// =============================================================================
// The seam
// =============================================================================

export fn ra8_imgdec_get_caps(dec: ?*const Handle, out: ?*Caps) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const handle = dec orelse return Err.null_ptr;
    return fabric.fetchCaps(handle, slot);
}

export fn ra8_imgdec_supports(
    dec: ?*const Handle,
    format: u32,
    pixel: u32,
    out_ok: ?*bool,
) callconv(.c) u16 {
    const slot = out_ok orelse return Err.null_ptr;
    const handle = dec orelse {
        slot.* = false;
        return Err.null_ptr;
    };
    return fabric.supports(handle, format, pixel, slot);
}

export fn ra8_imgdec_decode(
    dec: ?*const Handle,
    req: ?*const Req,
    out: ?*Image,
) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const handle = dec orelse return Err.null_ptr;
    const request = req orelse {
        slot.* = .{};
        return Err.null_ptr;
    };
    return fabric.decode(handle, request, slot, ops);
}

export fn ra8_imgdec_probe(
    dec: ?*const Handle,
    bytes: ?[*]const u8,
    byte_count: u32,
    out: ?*Geom,
) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    slot.* = .{};
    const handle = dec orelse return Err.null_ptr;
    const raw = bytes orelse return Err.null_ptr;
    return fabric.probe(handle, raw[0..byte_count], slot);
}

// =============================================================================
// Scratch
// =============================================================================

export fn ra8_imgdec_scratch_init(
    scratch: ?*Scratch,
    store: ?*anyopaque,
    cap: usize,
) callconv(.c) u16 {
    const slot = scratch orelse return Err.invalid_arg;
    const raw = store orelse return Err.invalid_arg;
    if (cap == 0) return Err.invalid_size;

    const bytes: [*]u8 = @ptrCast(raw);
    return scratch_mod.init(slot, bytes[0..cap]);
}

export fn ra8_imgdec_scratch_reset(scratch: ?*Scratch) callconv(.c) void {
    const slot = scratch orelse return;
    scratch_mod.reset(slot);
}

export fn ra8_imgdec_scratch_alloc(scratch: ?*Scratch, bytes: usize) callconv(.c) ?*anyopaque {
    const slot = scratch orelse return null;
    return @ptrCast(scratch_mod.alloc(slot, bytes));
}

export fn ra8_imgdec_scratch_calloc(
    scratch: ?*Scratch,
    count: usize,
    size: usize,
) callconv(.c) ?*anyopaque {
    const slot = scratch orelse return null;
    return @ptrCast(scratch_mod.calloc(slot, count, size));
}

export fn ra8_imgdec_scratch_realloc(
    scratch: ?*Scratch,
    ptr: ?*anyopaque,
    old_bytes: usize,
    new_bytes: usize,
) callconv(.c) ?*anyopaque {
    const slot = scratch orelse return null;
    const old: ?[*]u8 = if (ptr) |raw| @ptrCast(raw) else null;
    return @ptrCast(scratch_mod.realloc(slot, old, old_bytes, new_bytes));
}

export fn ra8_imgdec_scratch_free(scratch: ?*Scratch, ptr: ?*anyopaque) callconv(.c) void {
    const slot = scratch orelse return;
    const block: ?[*]u8 = if (ptr) |raw| @ptrCast(raw) else null;
    scratch_mod.free(slot, block);
}

export fn ra8_imgdec_scratch_high_water(scratch: ?*const Scratch) callconv(.c) usize {
    const slot = scratch orelse return 0;
    return scratch_mod.highWater(slot);
}

export fn ra8_imgdec_scratch_carve(
    scratch: ?*Scratch,
    arena_ptr: ?*anyopaque,
    bytes: u32,
    alignment: u32,
) callconv(.c) u16 {
    const slot = scratch orelse return Err.invalid_arg;
    if (arena_ptr == null) return Err.invalid_arg;

    const want = switch (scratch_mod.carveAlign(bytes, alignment)) {
        .fault => |code| return code,
        .ok => |value| value,
    };

    var block: ?*anyopaque = null;
    const err = ops.carve(arena_ptr, bytes, want, &block);
    if (err != Err.ok) return err;

    const raw: [*]u8 = @ptrCast(block orelse return Err.no_mem);
    return scratch_mod.init(slot, raw[0..bytes]);
}

// =============================================================================
// Mux
// =============================================================================

export fn ra8_imgdec_mux_init(mux: ?*Mux) callconv(.c) u16 {
    const slot = mux orelse return Err.null_ptr;
    return mux_mod.init(slot);
}

export fn ra8_imgdec_mux_add(mux: ?*Mux, dec: ?*const Handle) callconv(.c) u16 {
    const slot = mux orelse return Err.null_ptr;
    const handle = dec orelse return Err.null_ptr;
    return mux_mod.add(slot, handle);
}

export fn ra8_imgdec_mux_formats(
    mux: ?*const Mux,
    pixel: u32,
    out_formats: ?*u32,
) callconv(.c) u16 {
    const slot = out_formats orelse return Err.null_ptr;
    const set = mux orelse return Err.null_ptr;
    return mux_mod.formats(set, pixel, slot);
}

export fn ra8_imgdec_mux_supports(
    mux: ?*const Mux,
    format: u32,
    pixel: u32,
    out_ok: ?*bool,
) callconv(.c) u16 {
    const slot = out_ok orelse return Err.null_ptr;
    const set = mux orelse {
        slot.* = false;
        return Err.null_ptr;
    };
    return mux_mod.supports(set, format, pixel, slot);
}

export fn ra8_imgdec_mux_route(
    mux: ?*const Mux,
    format: u32,
    pixel: u32,
    out: ?*?*const Handle,
) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const set = mux orelse {
        slot.* = null;
        return Err.null_ptr;
    };
    return mux_mod.route(set, format, pixel, slot);
}

export fn ra8_imgdec_mux_decode(
    mux: ?*const Mux,
    req: ?*const Req,
    out: ?*Image,
) callconv(.c) u16 {
    const slot = out orelse return Err.null_ptr;
    const request = req orelse {
        slot.* = .{};
        return Err.null_ptr;
    };
    const set = mux orelse {
        slot.* = .{};
        return Err.null_ptr;
    };
    return mux_mod.decode(set, request, slot, ops);
}

export fn ra8_imgdec_mux_scratch_budget(
    mux: ?*const Mux,
    out_bytes: ?*u32,
    out_align: ?*u32,
) callconv(.c) u16 {
    const bytes_slot = out_bytes orelse return Err.null_ptr;
    const align_slot = out_align orelse return Err.null_ptr;
    const set = mux orelse return Err.null_ptr;
    return mux_mod.scratchBudget(set, bytes_slot, align_slot);
}

export fn ra8_imgdec_mux_carve(
    mux: ?*const Mux,
    arena_ptr: ?*anyopaque,
    out: ?*Scratch,
) callconv(.c) u16 {
    if (arena_ptr == null) return Err.null_ptr;
    const slot = out orelse return Err.null_ptr;
    const set = mux orelse return Err.null_ptr;

    var bytes: u32 = 0;
    var alignment: u32 = 0;
    const budget = mux_mod.scratchBudget(set, &bytes, &alignment);
    if (budget != Err.ok) return budget;

    // Nothing in the set decodes with scratch.
    if (bytes == 0) return Err.ok;

    return ra8_imgdec_scratch_carve(slot, arena_ptr, bytes, alignment);
}

export fn ra8_imgdec_mux_probe(
    mux: ?*const Mux,
    bytes: ?[*]const u8,
    byte_count: u32,
    pixel: u32,
    out_geom: ?*Geom,
    out_member: ?*?*const Handle,
) callconv(.c) u16 {
    const geom_slot = out_geom orelse return Err.null_ptr;
    geom_slot.* = .{};
    if (out_member) |slot| slot.* = null;

    const raw = bytes orelse return Err.null_ptr;
    const set = mux orelse return Err.null_ptr;

    return mux_mod.probe(set, raw[0..byte_count], pixel, geom_slot, out_member);
}
