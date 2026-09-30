//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_decomp_limits.h` (#2862).
//!
//! The units behind this file deal in policies, budgets and Zig errors.
//! This one maps them onto the `ra8_err_t` codes the header promises and
//! emits the same null-pointer log lines `RA8_CHECK_NULL_PTR` emitted,
//! through `ra8_log_emit_error` (Zig since #2836).
//!
//! Every entry point stays STRONG: unlike the log emitters and the
//! timebase, nothing in the tree overrides a bound check, and a weak
//! policy is a policy an image can silently lose.

const policy = @import("decomp_policy");
const budget = @import("decomp_budget");
const zip = @import("decomp_zip_eocd");

/// `ra8_err_t` values this module returns, from `inc/ra8_err.h`.
const err = struct {
    pub const ok: c_int = 0;
    pub const invalid_arg: c_int = 0x103;
    pub const null_ptr: c_int = 0x504;
    pub const decomp_output_cap: c_int = 0x505;
    pub const decomp_ratio: c_int = 0x506;
    pub const decomp_entries: c_int = 0x507;
    pub const decomp_depth: c_int = 0x508;
    pub const decomp_iterations: c_int = 0x509;
};

const tag: [*:0]const u8 = "ra8_decomp";

extern fn ra8_log_emit_error(tag: [*:0]const u8, message: [*:0]const u8) void;

fn errorFor(breach: budget.Breach) c_int {
    return switch (breach) {
        budget.Breach.OutputCap => err.decomp_output_cap,
        budget.Breach.Ratio => err.decomp_ratio,
        budget.Breach.Entries => err.decomp_entries,
        budget.Breach.Iterations => err.decomp_iterations,
        budget.Breach.Depth => err.decomp_depth,
    };
}

fn rejectNull(message: [*:0]const u8) c_int {
    ra8_log_emit_error(tag, message);
    return err.null_ptr;
}

pub export fn ra8_decomp_limits_default() callconv(.c) policy.Limits {
    return policy.default();
}

pub export fn ra8_decomp_budget_init(
    b: ?*budget.Budget,
    limits: ?*const policy.Limits,
) callconv(.c) c_int {
    const tracker = b orelse return rejectNull("budget init: null budget");
    tracker.* = .{};

    const bound = limits orelse {
        tracker.limits = policy.default();
        return err.ok;
    };
    if (!bound.usable()) return err.invalid_arg;
    tracker.limits = bound.*;
    return err.ok;
}

pub export fn ra8_decomp_budget_charge_output(
    b: ?*budget.Budget,
    in_total: u64,
    out_delta: u64,
) callconv(.c) c_int {
    const tracker = b orelse return rejectNull("charge output: null budget");
    tracker.chargeOutput(in_total, out_delta) catch |breach| return errorFor(breach);
    return err.ok;
}

pub export fn ra8_decomp_budget_charge_entry(b: ?*budget.Budget) callconv(.c) c_int {
    const tracker = b orelse return rejectNull("charge entry: null budget");
    tracker.chargeEntry() catch |breach| return errorFor(breach);
    return err.ok;
}

pub export fn ra8_decomp_budget_charge_iter(b: ?*budget.Budget) callconv(.c) c_int {
    const tracker = b orelse return rejectNull("charge iter: null budget");
    tracker.chargeIter() catch |breach| return errorFor(breach);
    return err.ok;
}

pub export fn ra8_decomp_budget_enter(b: ?*budget.Budget) callconv(.c) c_int {
    const tracker = b orelse return rejectNull("enter: null budget");
    tracker.enter() catch |breach| return errorFor(breach);
    return err.ok;
}

pub export fn ra8_decomp_budget_leave(b: ?*budget.Budget) callconv(.c) void {
    const tracker = b orelse return;
    tracker.leave();
}

pub export fn ra8_decomp_check_declared(
    limits: ?*const policy.Limits,
    comp_size: u64,
    out_size: u64,
) callconv(.c) c_int {
    const bound = limits orelse return rejectNull("check declared: null limits");
    budget.checkDeclared(bound.*, comp_size, out_size) catch |breach| return errorFor(breach);
    return err.ok;
}

/// `ra8_decomp_read_fn`: the positioned reader the preflight is handed.
const ReadFn = *const fn (ctx: ?*anyopaque, offset: u64, buf: ?*anyopaque, len: usize) callconv(.c) usize;

/// The C callback and its context, bridged to the slice-shaped reader the
/// scan unit takes. A function pointer cannot close over the pair, so the
/// pair travels as the scan's opaque context instead.
const Bridge = struct {
    read: ReadFn,
    ctx: ?*anyopaque,

    fn call(opaque_self: ?*anyopaque, dst: []u8, offset: u64) usize {
        const self: *const Bridge = @ptrCast(@alignCast(opaque_self.?));
        return self.read(self.ctx, offset, dst.ptr, dst.len);
    }
};

pub export fn ra8_decomp_zip_entry_preflight(
    read: ?ReadFn,
    ctx: ?*anyopaque,
    archive_size: u64,
) callconv(.c) c_int {
    const callback = read orelse return rejectNull("zip preflight: null reader");

    var bridge = Bridge{ .read = callback, .ctx = ctx };
    var scratch: [zip.scratch_bytes]u8 = undefined;
    const reader = zip.Reader{ .ctx = &bridge, .read = Bridge.call };

    return switch (zip.preflight(reader, archive_size, &scratch, policy.defaults.max_entries)) {
        .inconclusive => err.ok,
        .over_entry_cap => err.decomp_entries,
    };
}
