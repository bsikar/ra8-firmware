//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The J35 camera adapter: throw the U15 latch to the parallel DVP, route the
//! CEU pins, pulse the sensor's reset, and publish RIIC1 as the SCCB bus the
//! sensor driver talks over.

const hal = @import("hal.zig");
const io_expander = @import("io_expander.zig");
const pins = @import("camera_pins.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;

pub const I2cBusOps = hal.I2cBusOps;

pub const Sccb = struct {
    /// RIIC1 serves J35 SCCB.
    pub const i2c_channel: u8 = 1;
};

/// U15 latch, SW4-6 ON, which is what selects the parallel camera path over
/// the MIPI one.
pub const Latch = struct {
    pub const sw46_output: u8 = 0xDF;
    pub const sw46_mask: u8 = 0x20;
};

/// Reset is level-driven, so both halves of the pulse are held.
pub const Reset = struct {
    pub const low_ms: u32 = 20;
    pub const high_ms: u32 = 20;
};

/// Backing store for the bound bus. One camera on this board, so one bus.
var camera_bus: hal.IoI2cBus = .{ .iface = null, .ctx = null };

/// Throw the U15 latch so the parallel DVP reaches the CEU.
pub fn selectParallel() u32 {
    return io_expander.applyMask(Latch.sw46_output, Latch.sw46_mask);
}

/// Route every CEU pin. Stops at the first refusal so a conflicting route is
/// reported against the pin that actually clashed.
pub fn routeParallelPins() u32 {
    for (pins.parallel) |pin| {
        const err = hal.ra8_pfs_route_peripheral(pin, vocab.Psel.ceu, pins.parallel_owner);
        if (err != Err.ok) return err;
    }
    return Err.ok;
}

/// Pulse the sensor's reset line low then high, holding each level long
/// enough for the OV5640 to see it.
pub fn reset() u32 {
    const init_err = hal.ra8_gpio_output_init(pins.rst, vocab.Level.low);
    if (init_err != Err.ok) return init_err;

    hal.ra8_delay_ms(Reset.low_ms);

    const write_err = hal.ra8_gpio_write(pins.rst, vocab.Level.high);
    if (write_err != Err.ok) return write_err;

    hal.ra8_delay_ms(Reset.high_ms);
    return Err.ok;
}

/// The sensor driver's delay hook. The context is unused; it exists because
/// the driver's callback shape carries one.
pub fn delayMs(ctx: ?*anyopaque, milliseconds: u32) void {
    _ = ctx;
    hal.ra8_delay_ms(milliseconds);
}

/// Bind RIIC1 and publish it as a generic bus the sensor driver can drive.
///
/// Both halves live in `ra8_io`, which an app is free not to link. They are
/// declared weak for exactly that reason, so an app without `ra8_io` gets a
/// refusal here rather than a link failure over a facility it never asked
/// for. Same shape `console_stream.zig` uses for its UART sink.
pub fn i2cOps(out: *I2cBusOps) u32 {
    const bind = hal.ra8_io_i2c_bus_bind_riic orelse return Err.not_supported;
    const as_ops = hal.ra8_io_i2c_bus_as_ops orelse return Err.not_supported;

    const bind_err = bind(&camera_bus, Sccb.i2c_channel);
    if (bind_err != Err.ok) return bind_err;

    return as_ops(&camera_bus, out);
}

/// Test seam: the bus this module binds into.
pub fn boundBus() *const hal.IoI2cBus {
    return &camera_bus;
}
