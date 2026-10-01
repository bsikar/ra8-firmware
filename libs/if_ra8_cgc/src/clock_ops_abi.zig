//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three `fw_if_clock` ops, over `ra8_cgc` and `ra8_mstp`.
//!
//! Every op resolves through the table first, so the rows proven by the host
//! test are the rows these use. The port facade has already rejected a null
//! output, an unbound handle and an out-of-range kind before control reaches
//! here, so these check only what the chip layer can still refuse.
//!
//! Gating is reference-counted, and that is deliberate: `ra8_mstp_enable` and
//! `ra8_mstp_disable` keep a per-bit reference count, so enabling an
//! already-running module succeeds without touching hardware and disabling one
//! another user still holds leaves it running. Passing that through unchanged
//! is right for a port whose whole purpose is that two portable drivers can
//! each ask for their own block without knowing about each other. The one
//! sharp edge is inherited: disabling a module whose count is already zero
//! returns `k_ra8_err_invalid_state`, because an unbalanced release is a bug
//! in the caller and not something to swallow.

const abi = @import("clock_map_abi");

const Err = abi.Err;
const Module = abi.Module;
const Row = abi.Row;

extern fn ra8_cgc_get_clock_hz(id: u8, out_hz: ?*u32) callconv(.c) u32;
extern fn ra8_mstp_enable(id: u16) callconv(.c) u32;
extern fn ra8_mstp_disable(id: u16) callconv(.c) u32;
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

/// Read the rate of the domain feeding one module. The RA8 clock tree is a
/// chip singleton, so the context is unused.
fn rateFor(ctx: ?*anyopaque, module: Module, out_hz: ?*u32) callconv(.c) u32 {
    _ = ctx;

    var row: Row = undefined;
    const err = abi.fill(module, &row);
    if (err != Err.ok) return err;
    if (!row.has_domain) return Err.not_supported;

    return ra8_cgc_get_clock_hz(row.domain, out_hz);
}

/// Ungate or gate one module's clock.
fn setGate(ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32 {
    _ = ctx;

    var row: Row = undefined;
    const err = abi.fill(module, &row);
    if (err != Err.ok) return err;
    if (!row.has_gate) return Err.not_supported;

    return if (on) ra8_mstp_enable(row.gate) else ra8_mstp_disable(row.gate);
}

/// Whether this adapter can resolve a module instance. Reports resolvability,
/// not silicon: a `false` says this binding carries no row, never that the
/// chip lacks the block.
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

/// Static storage with no context, because the RA8 clock tree and module-stop
/// block are chip singletons. The pointer outlives any handle bound to it.
const iface: Iface = .{
    .rate_for = &rateFor,
    .set_gate = &setGate,
    .has_module = &hasModule,
};

pub export fn fw_clock_ra8_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_clock_ra8_bind(clk: ?*Handle) callconv(.c) u32 {
    return fw_clock_bind(clk, &iface, null);
}
