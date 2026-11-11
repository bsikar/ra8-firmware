//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Test-link stubs for the longstrip Zig unit tests (RA8FW-1156): the JOF
//! reader, the tile cache and the RA8_CHECK log backend the engine drives.
//! They ride only the standalone test link -- they never enter
//! liblongstrip.a or any consumer link. Each one is a small deterministic
//! fake; the exported ls_stub_* variables are the tests' control surface.
const std = @import("std");
const ls_h = @import("longstrip_h");

// Test control: 1 makes the fake atlas multi-column (open must reject it),
// 1 makes every cache get miss, and ls_stub_puts counts successful puts.
export var ls_stub_multi_column: c_int = 0;
export var ls_stub_cache_miss: c_int = 0;
export var ls_stub_puts: c_int = 0;

const fake_w: u32 = 200;
const fake_h: u32 = 1000;
const fake_tile_w: u16 = 200;
const fake_tile_h: u16 = 100;
const fake_bpp: u8 = 4;

var fake_cell: [fake_w * fake_tile_h * 4]u8 = undefined;

pub export fn ra8_log_emit_error(tag: [*c]const u8, message: [*c]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
}

pub export fn jof_parse(pread: ls_h.jof_pread_fn, pread_ctx: ?*anyopaque, total_size: u64, out_info: ?*ls_h.jof_info_t) callconv(.c) ls_h.ra8_err_t {
    _ = pread;
    _ = pread_ctx;
    _ = total_size;
    const info = out_info orelse return ls_h.k_ra8_err_null_ptr;
    info.* = std.mem.zeroes(ls_h.jof_info_t);
    info.width = fake_w;
    info.height = fake_h;
    if (ls_stub_multi_column != 0) {
        info.tile_w = 100;
    } else {
        info.tile_w = fake_tile_w;
    }
    info.tile_h = fake_tile_h;
    info.bpp = fake_bpp;
    info.tile_cols = 1;
    info.tile_rows = 10;
    return ls_h.k_ra8_ok;
}

pub export fn jof_read_tile(pread: ls_h.jof_pread_fn, pread_ctx: ?*anyopaque, info: ?*const ls_h.jof_info_t, tile_x: u16, tile_y: u16, scratch: ?[*]u8, scratch_cap: u32, out_px: ?[*]u8, out_cap: u32, out_w: ?*u16, out_h: ?*u16) callconv(.c) ls_h.ra8_err_t {
    _ = pread;
    _ = pread_ctx;
    _ = info;
    _ = tile_x;
    _ = tile_y;
    _ = scratch;
    _ = scratch_cap;
    _ = out_cap;
    out_w.?.* = fake_tile_w;
    out_h.?.* = fake_tile_h;
    for (fake_cell[0..fake_cell.len], 0..) |*b, i| {
        b.* = @truncate(i);
    }
    @memcpy(out_px.?[0..fake_cell.len], &fake_cell);
    return ls_h.k_ra8_ok;
}

pub export fn ra8_tile_cache_get(tc: ?*ls_h.ra8_tile_cache_t, key: ?*const ls_h.ra8_tile_key_t, out_tile: ?*ls_h.ra8_tile_t) callconv(.c) ls_h.ra8_err_t {
    _ = tc;
    _ = key;
    if (ls_stub_cache_miss != 0) {
        return ls_h.k_ra8_err_out_of_range;
    }
    const tile = out_tile orelse return ls_h.k_ra8_err_null_ptr;
    tile.* = std.mem.zeroes(ls_h.ra8_tile_t);
    tile.pixels = &fake_cell;
    tile.width = fake_tile_w;
    tile.height = fake_tile_h;
    return ls_h.k_ra8_ok;
}

pub export fn ra8_tile_cache_put(tc: ?*ls_h.ra8_tile_cache_t, pixels: ?*const u8) callconv(.c) ls_h.ra8_err_t {
    _ = tc;
    if (pixels == null) {
        return ls_h.k_ra8_err_null_ptr;
    }
    ls_stub_puts += 1;
    return ls_h.k_ra8_ok;
}
