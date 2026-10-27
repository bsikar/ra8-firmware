//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three `fw_if_clock` ops, over the stub profile.
//!
//! Every op resolves through the table first, so the rows proven by the host
//! test are the rows these use. The port facade has already rejected a null
//! output, an unbound handle and an out-of-range kind before control reaches
//! here, so these check only what the table can still refuse.
//!
//! Gating in this binding succeeds and does nothing: a stub board has no
//! clock tree to program, so a carried module's enable and disable both
//! return ok with no side effect, and an absent one returns `not_found`, the
//! same answer ::fw_clock_rate_for gives. The port's not-supported code is
//! reserved for a binding that cannot gate a kind at all, which is not what a
//! complete-but-hardwareless profile is.

const abi = @import("clock_stub_abi");

const Err = abi.Err;
const Module = abi.Module;
const Row = abi.Row;

extern fn fw_clock_bind(clk: ?*Handle, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u32;

/// `fw_clock_iface_t`. A binding that cannot gate a module returns
/// not-supported from `set_gate` rather than leaving the pointer null: a null
/// op is a malformed binding, not a declined capability.
pub const Iface = extern struct {
    rate_for: *const fn (ctx: ?*anyopaque, module: Module, out_hz: ?*u32) callconv(.c) u32,
    set_gate: *const fn (ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32,
    has_module: *const fn (ctx: ?*anyopaque, module: Module, out_present: ?*bool) callconv(.c) u32,
};

/// `fw_clock_t`, caller-owned and opaque once `fw_clock_bind` has filled it.
pub const Handle = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

/// Read the rate of the clock feeding one module. The stub profile is a
/// compile-time constant, so the context is unused.
fn rateFor(ctx: ?*anyopaque, module: Module, out_hz: ?*u32) callconv(.c) u32 {
    _ = ctx;

    const out = out_hz orelse return Err.invalid_arg;

    var row: Row = undefined;
    const err = abi.fill(module, &row);
    if (err != Err.ok) return err;

    out.* = row.rate_hz;
    return Err.ok;
}

/// Ungate or gate one module's clock. Succeeds without side effect for a
/// carried module.
fn setGate(ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32 {
    _ = ctx;
    _ = on;

    var row: Row = undefined;
    const err = abi.fill(module, &row);
    if (err != Err.ok) return err;

    return Err.ok;
}

/// Whether this binding can resolve a module instance. Reports
/// resolvability, not silicon: a `false` says this binding carries no row,
/// never that a real chip would lack the block.
fn hasModule(ctx: ?*anyopaque, module: Module, out_present: ?*bool) callconv(.c) u32 {
    _ = ctx;

    const present = out_present orelse return Err.invalid_arg;

    var row: Row = undefined;
    const err = abi.fill(module, &row);
    if (err == Err.invalid_arg) {
        present.* = false;
        return Err.invalid_arg;
    }

    present.* = (err == Err.ok);
    return Err.ok;
}

/// Static storage with no context, because the stub profile is a singleton.
/// The pointer outlives any handle bound to it.
const iface: Iface = .{
    .rate_for = &rateFor,
    .set_gate = &setGate,
    .has_module = &hasModule,
};

pub export fn fw_clock_stub_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_clock_stub_bind(clk: ?*Handle) callconv(.c) u32 {
    return fw_clock_bind(clk, &iface, null);
}
