//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_clock.h`: the `fw_clock` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_clock_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, call the binding, and refuse a binding answer that cannot be true.
//!
//! That last one is why this file exists rather than the header being the
//! whole port. A rate of zero is not a rate; a binding that answers ok with
//! `*out_hz == 0` has a bug, and a driver that divides by it gets undefined
//! behaviour several frames later in code that is not the buggy code.
//! Catching it at the seam names the call that caused it instead.
//!
//! `fw_clock_require` is the port's only policy, and it is deliberately a
//! comparison rather than a search: raising a shared bus clock is not a
//! portable driver's decision to make.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_clock_module_kind_t`, `uint8_t`-backed as the header declares it.
pub const ModuleKind = struct {
    pub const none: u8 = 0;
    pub const count: u8 = 20;
};

/// `fw_clock_module_t`: which block a request is about.
pub const Module = extern struct {
    kind: u8,
    index: u8,
};

/// `fw_clock_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    rate_for: ?*const fn (?*anyopaque, Module, ?*u32) callconv(.c) Err,
    set_gate: ?*const fn (?*anyopaque, Module, bool) callconv(.c) Err,
    has_module: ?*const fn (?*anyopaque, Module, ?*bool) callconv(.c) Err,
};

/// `fw_clock_t`: the caller-owned binding handle.
pub const Clock = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the enum to uint8_t and the handle to a plain C bool; both
// are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(Module) == 2);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a module names a kind this contract enumerates.
///
/// `k_fw_clock_module_none` is rejected along with anything past the last
/// enumerator: a zeroed `fw_clock_module_t` is the shape a caller gets by
/// forgetting to fill one in, and answering it would hide that mistake. The
/// index is not range-checked here: how many instances exist is a board fact,
/// so `has_module` owns it.
fn kindValid(module: Module) bool {
    return module.kind != ModuleKind.none and module.kind < ModuleKind.count;
}

/// Common entry guard: bound handle, enumerated kind.
fn check(clk: ?*const Clock, module: Module) Err {
    const handle = clk orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    if (!kindValid(module)) return core.err_invalid_arg;
    return core.ok;
}

pub export fn fw_clock_bind(clk: ?*Clock, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = clk orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot gate a module says so from set_gate.
    if (ops.rate_for == null or ops.set_gate == null or ops.has_module == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_clock_rate_for(clk: ?*const Clock, module: Module, out_hz: ?*u32) callconv(.c) Err {
    const out = out_hz orelse return core.err_invalid_arg;
    out.* = 0;

    const guard = check(clk, module);
    if (guard != core.ok) return guard;

    var hz: u32 = 0;
    const handle = clk.?;
    const err = handle.iface.?.rate_for.?(handle.ctx, module, &hz);
    if (err != core.ok) return err;
    // Zero is not a rate. Refusing here names the buggy binding instead of
    // leaving a division by zero in whichever driver asked.
    if (hz == 0) return core.err_invalid_state;
    out.* = hz;
    return core.ok;
}

pub export fn fw_clock_require(
    clk: ?*const Clock,
    module: Module,
    min_hz: u32,
    out_hz: ?*u32,
) callconv(.c) Err {
    if (min_hz == 0) {
        // A floor of zero asks nothing; the caller wanted fw_clock_rate_for.
        if (out_hz) |out| out.* = 0;
        return core.err_invalid_arg;
    }

    const err = fw_clock_rate_for(clk, module, out_hz);
    if (err != core.ok) return err;
    // The rate stays written: a driver reports what it found rather than
    // guessing, and the board decides whether to reprogram the tree.
    if (out_hz.?.* < min_hz) return core.err_not_supported;
    return core.ok;
}

pub export fn fw_clock_enable(clk: ?*const Clock, module: Module) callconv(.c) Err {
    const guard = check(clk, module);
    if (guard != core.ok) return guard;
    const handle = clk.?;
    return handle.iface.?.set_gate.?(handle.ctx, module, true);
}

pub export fn fw_clock_disable(clk: ?*const Clock, module: Module) callconv(.c) Err {
    const guard = check(clk, module);
    if (guard != core.ok) return guard;
    const handle = clk.?;
    return handle.iface.?.set_gate.?(handle.ctx, module, false);
}

pub export fn fw_clock_has_module(
    clk: ?*const Clock,
    module: Module,
    out_present: ?*bool,
) callconv(.c) Err {
    const out = out_present orelse return core.err_invalid_arg;
    out.* = false;

    const guard = check(clk, module);
    if (guard != core.ok) return guard;

    var present = false;
    const handle = clk.?;
    const err = handle.iface.?.has_module.?(handle.ctx, module, &present);
    if (err != core.ok) return err;
    out.* = present;
    return core.ok;
}
