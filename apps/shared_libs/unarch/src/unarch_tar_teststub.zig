//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Package-test stand-in for the decompression-policy seam
//! (libs/ra8_core's ra8_decomp_budget_* / ra8_decomp_check_declared). The
//! production seam lives in ra8_core and links at the app level; the package
//! test binary links this stub instead so the walker's budget calls resolve.
//! It approves every budget and never enforces limits.

const std = @import("std");

pub export fn ra8_decomp_budget_init(b: ?*anyopaque, limits: ?*const anyopaque) callconv(.c) c_int {
    _ = b;
    _ = limits;
    return 0;
}

pub export fn ra8_decomp_budget_charge_entry(b: ?*anyopaque) callconv(.c) c_int {
    _ = b;
    return 0;
}

pub export fn ra8_decomp_budget_charge_iter(b: ?*anyopaque) callconv(.c) c_int {
    _ = b;
    return 0;
}

pub export fn ra8_decomp_check_declared(limits: ?*const anyopaque, comp_size: u64, out_size: u64) callconv(.c) c_int {
    _ = limits;
    _ = comp_size;
    _ = out_size;
    return 0;
}

comptime {
    _ = std;
}
