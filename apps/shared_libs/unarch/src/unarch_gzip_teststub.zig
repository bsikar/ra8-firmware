//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Package-test stand-in for the ra8_core decompression budget seam
//! (unarch_gzip_abi.zig links these as externs; the real lives in
//! libs/ra8_core/src/internal/decomp). Same idiom as the tar teststub:
//! a small real budget enforcing the limits it is handed, so tests can
//! drive init validation and charging without linking ra8_core.

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
const k_ra8_err_decomp_output: c_int = 0x201;
const k_ra8_err_decomp_iter: c_int = 0x202;

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
        return k_ra8_err_decomp_iter;
    }
    return k_ra8_ok;
}

export fn ra8_decomp_budget_charge_output(b: ?*anyopaque, in_pos: u64, out_bytes: u64) callconv(.c) c_int {
    if (b == null) {
        return k_ra8_err_invalid_arg;
    }
    _ = in_pos;
    s_budget_out += out_bytes;
    if (s_budget_out > s_budget_limits.max_output_bytes) {
        return k_ra8_err_decomp_output;
    }
    return k_ra8_ok;
}
