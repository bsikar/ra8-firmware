//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! GT911 bring-up on the board's own wiring: solve the bit rate against the
//! live PCLKA, bring IIC_B up in I2C-compatibility mode, and hand the bound
//! bus to the touch driver.

const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

const Err = vocab.Err;
const Touch = vocab.Touch;

/// What an application asks for. The board supplies everything else.
pub const Cfg = extern struct {
    max_points: u8,
    irq_pin: u8,
};

/// Backing store for the bound bus. The driver keeps the ops it is handed and
/// those ops point back here, so this must out-live the open driver. One
/// GT911 on one board, so one handle is the whole requirement.
var touch_bus: hal.IoI2cBus = .{ .iface = null, .ctx = null };

/// Zero means "board default", anything above the driver's cap is a refusal
/// rather than a silent clamp.
fn resolvePoints(requested: u8) ?u8 {
    if (requested > Touch.max_points) return null;
    return if (requested == 0) Touch.max_points else requested;
}

/// Bring the bus up at the requested rate against the clock it actually runs
/// on. Eight copies of this block used to pass a hardcoded 60 MHz while the
/// CGC tree publishes 125 MHz, so none of them reached the 400 kHz they asked
/// for.
fn busInit() u32 {
    var pclka_hz: u32 = 0;
    const clk_err = hal.ra8_cgc_get_clock_hz(vocab.ClockId.pclka, &pclka_hz);
    if (clk_err != Err.ok) return clk_err;

    const cfg: hal.I3cCfg = .{
        .mode = vocab.I3c.mode_i2c,
        .bus_hz = Touch.bus_hz,
        .pclka_hz = pclka_hz,
    };
    return hal.ra8_i3c_init(Touch.i3c_channel, &cfg);
}

/// Bind IIC_B and publish it as the generic bus the driver drives.
///
/// Both halves live in `ra8_io`, which an app is free not to link, so both
/// are weak and an app without them gets a refusal instead of a link failure.
fn bindBus(out: *hal.I2cBusOps) u32 {
    const bind = hal.ra8_io_i2c_bus_bind_i3c_compat orelse return Err.not_supported;
    const as_ops = hal.ra8_io_i2c_bus_as_ops orelse return Err.not_supported;

    const bind_err = bind(&touch_bus, Touch.i3c_channel);
    if (bind_err != Err.ok) return bind_err;

    return as_ops(&touch_bus, out);
}

/// The four-call sequence eight applications used to write out by hand.
pub fn open(cfg: *const Cfg) u32 {
    const max_points = resolvePoints(cfg.max_points) orelse return Err.invalid_arg;

    const bus_err = busInit();
    if (bus_err != Err.ok) return bus_err;

    var bus_ops: hal.I2cBusOps = .{ .write = null, .read = null, .transfer = null, .ctx = null };
    const ops_err = bindBus(&bus_ops);
    if (ops_err != Err.ok) return ops_err;

    const touch_cfg: hal.TouchCfg = .{
        .bus = bus_ops,
        .target_7b = Touch.target_7b,
        .irq_pin = cfg.irq_pin,
        .max_points = max_points,
    };
    return hal.ra8_touch_open(&touch_cfg);
}

/// Test seam: the bus this module binds into.
pub fn boundBus() *const hal.IoI2cBus {
    return &touch_bus;
}

/// Test seam: the point-cap policy, without a bus in the way.
pub fn resolvedPoints(requested: u8) ?u8 {
    return resolvePoints(requested);
}
