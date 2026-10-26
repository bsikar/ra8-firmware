//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_uart.h`: the `fw_uart` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_uart_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. UART indexes are a board fact, so the
//! membrane does not range-check them; the binding owns that and answers
//! `k_ra8_err_not_found` for one it does not carry. The line description is
//! the one thing the membrane refuses on its own account: a zero baud is not
//! a baud rate, and a zeroed `fw_uart_config_t` is the shape a caller gets by
//! forgetting to fill one in.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_uart_data_bits_t`, `uint8_t`-backed as the header declares it.
pub const DataBits = struct {
    pub const none: u8 = 0;
    pub const seven: u8 = 1;
    pub const eight: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_uart_parity_t`, `uint8_t`-backed as the header declares it.
pub const Parity = struct {
    pub const none: u8 = 0;
    pub const even: u8 = 1;
    pub const odd: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_uart_stop_bits_t`, `uint8_t`-backed as the header declares it.
pub const StopBits = struct {
    pub const none: u8 = 0;
    pub const one: u8 = 1;
    pub const two: u8 = 2;
    pub const count: u8 = 3;
};

/// `fw_uart_config_t`: baud, width, parity and stop bits.
pub const Config = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
};

/// `fw_uart_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    configure: ?*const fn (?*anyopaque, u8, Config) callconv(.c) Err,
    write: ?*const fn (?*anyopaque, u8, ?[*]const u8, usize) callconv(.c) Err,
    read: ?*const fn (?*anyopaque, u8, ?[*]u8, usize, ?*usize) callconv(.c) Err,
};

/// `fw_uart_t`: the caller-owned binding handle.
pub const Uart = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the config's numeric width and the handle's flag to plain C
// uint32_t/uint8_t and C bool; both are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(Config) == 8);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a line description names fields this contract enumerates. A zero
/// baud is refused along with anything past the last enumerator of each
/// field; parity's `none` is a real answer and is not a rejection.
fn cfgValid(cfg: Config) bool {
    const baud_ok = cfg.baud != 0;
    const width_ok = cfg.data_bits != DataBits.none and cfg.data_bits < DataBits.count;
    const parity_ok = cfg.parity < Parity.count;
    const stop_ok = cfg.stop_bits != StopBits.none and cfg.stop_bits < StopBits.count;
    return baud_ok and width_ok and parity_ok and stop_ok;
}

/// Common entry guard: bound handle.
fn check(uart: ?*const Uart) Err {
    const handle = uart orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}

pub export fn fw_uart_bind(uart: ?*Uart, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = uart orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot receive says so from read, not by leaving it unset.
    if (ops.configure == null or ops.write == null or ops.read == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_uart_configure(uart: ?*const Uart, uart_index: u8, cfg: Config) callconv(.c) Err {
    if (!cfgValid(cfg)) return core.err_invalid_arg;

    const guard = check(uart);
    if (guard != core.ok) return guard;

    const handle = uart.?;
    return handle.iface.?.configure.?(handle.ctx, uart_index, cfg);
}

pub export fn fw_uart_write(uart: ?*const Uart, uart_index: u8, bytes: ?[*]const u8, len: usize) callconv(.c) Err {
    // A zero-byte write asks for nothing; it is satisfied without waking the
    // peripheral, and `bytes` may be NULL because nothing is read.
    if (len == 0) return core.ok;
    const buffer = bytes orelse return core.err_invalid_arg;

    const guard = check(uart);
    if (guard != core.ok) return guard;

    const handle = uart.?;
    return handle.iface.?.write.?(handle.ctx, uart_index, buffer, len);
}

pub export fn fw_uart_read(uart: ?*const Uart, uart_index: u8, out: ?[*]u8, len: usize, out_count: ?*usize) callconv(.c) Err {
    const count_out = out_count orelse return core.err_invalid_arg;
    count_out.* = 0;
    // A zero-capacity poll asks for nothing; ok with a zero count.
    if (len == 0) return core.ok;
    const buffer = out orelse return core.err_invalid_arg;

    const guard = check(uart);
    if (guard != core.ok) return guard;

    var count: usize = 0;
    const handle = uart.?;
    const err = handle.iface.?.read.?(handle.ctx, uart_index, buffer, len, &count);
    if (err != core.ok) return err;
    // A count above the capacity is a lying binding, not a big read.
    if (count > len) return core.err_invalid_state;
    count_out.* = count;
    return core.ok;
}
