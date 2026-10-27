//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three `fw_if_uart` binding ops, over the stub profile.
//!
//! The port facade has already rejected a null output count, a null buffer
//! with a nonzero length and an unbound handle before control reaches here,
//! so these check only what the profile itself can still refuse: a line the
//! board does not carry, a malformed line description.
//!
//! Static storage with no context, because the stub serves one port: the ops
//! ignore the context the port hands them.

const stub = @import("uart_stub");

/// `ra8_err.h` codes these ops can return.
pub const Err = struct {
    pub const ok: u32 = 0;
    pub const invalid_arg: u32 = 0x103;
    pub const not_found: u32 = 0x106;
};

extern fn fw_uart_bind(uart: ?*Handle, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) u32;

/// `fw_uart_config_t`, as the C header lays it out (u32 then three u8s).
pub const Config = extern struct {
    baud: u32,
    data_bits: u8,
    parity: u8,
    stop_bits: u8,
};

/// `fw_uart_iface_t`. All three ops are required by the facade's bind, so no
/// pointer is optional here.
pub const Iface = extern struct {
    configure: *const fn (ctx: ?*anyopaque, uart: u8, cfg: Config) callconv(.c) u32,
    write: *const fn (ctx: ?*anyopaque, uart: u8, bytes: ?[*]const u8, len: usize) callconv(.c) u32,
    read: *const fn (ctx: ?*anyopaque, uart: u8, out: ?[*]u8, len: usize, out_count: ?*usize) callconv(.c) u32,
};

/// `fw_uart_t`, caller-owned and opaque once `fw_uart_bind` has filled it.
pub const Handle = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

/// Put one line on a line description. A line the board does not carry is
/// `not_found`, the same answer the port facade gives.
pub export fn fw_uart_stub_configure(ctx: ?*anyopaque, uart: u8, cfg: Config) callconv(.c) u32 {
    _ = ctx;

    stub.configure(uart, .{
        .baud = cfg.baud,
        .data_bits = cfg.data_bits,
        .parity = cfg.parity,
        .stop_bits = cfg.stop_bits,
    }) catch |err| {
        return switch (err) {
            error.InvalidArg => Err.invalid_arg,
            error.NotFound => Err.not_found,
        };
    };

    return Err.ok;
}

/// Transmit bytes on one line, captured into its ring. A null buffer with a
/// nonzero length is `invalid_arg`, matching the facade's own guard; a
/// zero-length write is satisfied without waking the peripheral, so a null
/// buffer with a zero length reaches neither the profile nor an error.
pub export fn fw_uart_stub_write(ctx: ?*anyopaque, uart: u8, bytes: ?[*]const u8, len: usize) callconv(.c) u32 {
    _ = ctx;

    if (bytes == null and len > 0) return Err.invalid_arg;

    if (bytes) |payload| {
        stub.write(uart, payload[0..len]) catch {
            return Err.not_found;
        };
    }

    return Err.ok;
}

/// Poll one line for received bytes. A null output count or a null buffer
/// with a nonzero length is `invalid_arg`, matching the facade's own guards;
/// a quiet line reads zero, not an error.
pub export fn fw_uart_stub_read(ctx: ?*anyopaque, uart: u8, out: ?[*]u8, len: usize, out_count: ?*usize) callconv(.c) u32 {
    _ = ctx;

    const count_ptr = out_count orelse return Err.invalid_arg;
    if (out == null and len > 0) return Err.invalid_arg;

    var count: usize = 0;
    if (out) |buffer| {
        count = stub.read(uart, buffer[0..len]) catch {
            return Err.not_found;
        };
    }

    count_ptr.* = count;
    return Err.ok;
}

/// Static storage with no context, because the stub profile is a singleton.
/// The pointer outlives any handle bound to it.
const iface: Iface = .{
    .configure = &fw_uart_stub_configure,
    .write = &fw_uart_stub_write,
    .read = &fw_uart_stub_read,
};

pub export fn fw_uart_stub_iface() callconv(.c) *const Iface {
    return &iface;
}

pub export fn fw_uart_stub_bind(uart: ?*Handle) callconv(.c) u32 {
    return fw_uart_bind(uart, &iface, null);
}
