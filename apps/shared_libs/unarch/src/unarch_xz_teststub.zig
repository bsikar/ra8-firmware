//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Package-test stand-in for the ra8_core decompression budget seam
//! (unarch_xz_abi.zig links these as externs; the real lives in
//! libs/ra8_core/src/internal/decomp). Same idiom as the gzip teststub:
//! a small real budget enforcing the limits it is handed, charging the
//! output cap and the ratio bound the way budget.zig does.

const std = @import("std");

const Limits = extern struct {
    max_output_bytes: u64 = 0,
    max_ratio: u32 = 0,
    ratio_grace_bytes: u32 = 0,
    max_entries: u32 = 0,
    max_iterations: u32 = 0,
    max_depth: u8 = 0,
};

const k_ra8_ok: c_int = 0;
const k_ra8_err_invalid_arg: c_int = 0x103;
const k_ra8_err_decomp_output_cap: c_int = 0x505;
const k_ra8_err_decomp_ratio: c_int = 0x506;
const k_ra8_err_decomp_iterations: c_int = 0x509;

const Defaults = Limits{
    .max_output_bytes = 1 << 30,
    .max_ratio = 1024,
    .ratio_grace_bytes = 4096,
    .max_entries = 1 << 16,
    .max_iterations = 1 << 20,
    .max_depth = 8,
};

var s_budget_out: u64 = 0;
var s_budget_iters: u32 = 0;
var s_budget_limits: Limits = .{};

export fn ra8_decomp_budget_init(b: ?*anyopaque, limits: ?*const Limits) callconv(.c) c_int {
    if (b == null) {
        return k_ra8_err_invalid_arg;
    }
    s_budget_limits = if (limits) |l| l.* else Defaults;
    if (s_budget_limits.max_output_bytes == 0 or s_budget_limits.max_iterations == 0) {
        return k_ra8_err_invalid_arg; // a zero policy field is a malformed request
    }
    s_budget_out = 0;
    s_budget_iters = 0;
    return k_ra8_ok;
}

export fn ra8_decomp_budget_charge_iter(b: ?*anyopaque) callconv(.c) c_int {
    if (b == null) {
        return k_ra8_err_invalid_arg;
    }
    s_budget_iters += 1;
    if (s_budget_iters > s_budget_limits.max_iterations) {
        return k_ra8_err_decomp_iterations;
    }
    return k_ra8_ok;
}

export fn ra8_decomp_budget_charge_output(b: ?*anyopaque, in_pos: u64, out_bytes: u64) callconv(.c) c_int {
    if (b == null) {
        return k_ra8_err_invalid_arg;
    }
    s_budget_out = std.math.add(u64, s_budget_out, out_bytes) catch std.math.maxInt(u64);
    if (s_budget_out > s_budget_limits.max_output_bytes) {
        return k_ra8_err_decomp_output_cap;
    }
    const product = std.math.mul(u64, in_pos, s_budget_limits.max_ratio) catch std.math.maxInt(u64);
    const bound = std.math.add(u64, product, s_budget_limits.ratio_grace_bytes) catch std.math.maxInt(u64);
    if (s_budget_out > bound) {
        return k_ra8_err_decomp_ratio;
    }
    return k_ra8_ok;
}

// ---- The shared decoder-scratch contract (RA8FW-308), test edition -------
// unarch_xz_pool.c forwards its allocation to the shared contract
// (libs/ra8_imgdec/src/internal/scratch.zig). The package test builds that
// contract here with the same published semantics: bump with the contract's
// 16-byte alignment, count live blocks, refuse rather than overrun, and
// drain on reset.

const scratch_align: usize = 16;

const Scratch = extern struct {
    base: ?[*]u8 = null,
    cap: usize = 0,
    offset: usize = 0,
    live: usize = 0,
    high_water: usize = 0,
};

export fn ra8_imgdec_scratch_init(s: ?*Scratch, base: ?*anyopaque, cap: usize) callconv(.c) u16 {
    if (s == null or base == null) {
        return 0x504; // null_ptr
    }
    if (cap == 0) {
        return 0x105; // invalid_size
    }
    s.?.* = .{ .base = @ptrCast(base.?), .cap = cap };
    return 0;
}

export fn ra8_imgdec_scratch_reset(s: ?*Scratch) callconv(.c) void {
    if (s == null) {
        return;
    }
    s.?.live = 0;
    s.?.offset = 0;
}

export fn ra8_imgdec_scratch_alloc(s: ?*Scratch, bytes: usize) callconv(.c) ?*anyopaque {
    if (s == null or bytes == 0) {
        return null;
    }
    const aligned = (s.?.offset + scratch_align - 1) & ~(scratch_align - 1);
    const end = aligned + bytes;
    if (s.?.base == null or end > s.?.cap) {
        return null; // refuse rather than overrun
    }
    const ptr = s.?.base.? + aligned;
    s.?.offset = end;
    s.?.live += 1;
    if (s.?.offset > s.?.high_water) {
        s.?.high_water = s.?.offset;
    }
    return ptr;
}

// The pool logs refusals through ra8_log.h; the test build silences them.
export fn ra8_log_emit_error(tag: ?[*:0]const u8, message: ?[*:0]const u8) callconv(.c) void {
    _ = tag;
    _ = message;
}
