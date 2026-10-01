//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What this board routes, per clock-module kind, and the `fw_if_clock`
//! binding over it. Board index is the subscript, chip instance is the value:
//! an explicit list rather than a base and a stride, because the wired SCI
//! channels are scattered (0, 2, 7, 8) and no arithmetic reproduces them.

const clock = @import("clock_types.zig");
const hal = @import("hal.zig");
const vocab = @import("vocab.zig");

pub const Err = vocab.Err;
pub const Module = clock.Module;
pub const Kind = clock.Kind;
pub const FwClock = clock.FwClock;
pub const Iface = clock.FwClockIface;

/// Longest wired run on this board, which is the four SCI channels.
const max_wired = 4;

/// One kind's wiring: board index -> chip instance, in board order.
const Wiring = struct {
    count: u8 = 0,
    chip_index: [max_wired]u8 = .{0} ** max_wired,
};

/// Each wired row takes its chip instance from the board constant that already
/// names that channel, so a connector that moves moves here with it.
///
/// uart 0..3 are SCI 0, 2, 7, 8: Pmod2 J25, Pmod1 J26, mikroBUS, and the
/// J-Link OB VCOM console. Ordered by chip instance so the numbering stays
/// stable as connectors are added, not by any notion of importance. i2c 0 is
/// RIIC1, the J35 camera SCCB bus; the touch panel is deliberately not a
/// second row, because it is I3C in I2C-compatible mode, a different block
/// with its own module-stop bit, and presenting it as I2C 1 would gate the
/// wrong peripheral. camera, display, ethernet and memory are single-instance
/// blocks, so board and chip numbering agree; the rows exist so the profile
/// answers for them rather than refusing. Absent on purpose because the board
/// routes none: spi (both Pmods take SPI from SCI in Simple-SPI mode), can,
/// adc, dac, usb, timer, pwm, dma, rtc, watchdog, crypto.
const wiring = blk: {
    var table = [_]Wiring{.{}} ** Kind.count;
    table[Kind.core] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    table[Kind.uart] = .{ .count = 4, .chip_index = .{ 0, 2, 7, 8 } };
    table[Kind.i2c] = .{ .count = 1, .chip_index = .{ 1, 0, 0, 0 } };
    table[Kind.sdhost] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    table[Kind.camera] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    table[Kind.display] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    table[Kind.ethernet] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    table[Kind.memory] = .{ .count = 1, .chip_index = .{ 0, 0, 0, 0 } };
    break :blk table;
};

fn kindValid(kind: u8) bool {
    return kind < Kind.count;
}

/// Translate a board-numbered module to the chip instance behind it.
pub fn toChip(module: Module, out_chip: ?*Module) u32 {
    const dst = out_chip orelse return Err.invalid_arg;
    if (!kindValid(module.kind)) return Err.not_found;
    const row = wiring[module.kind];
    if (module.index >= row.count) return Err.not_found;
    dst.* = .{ .kind = module.kind, .index = row.chip_index[module.index] };
    return Err.ok;
}

/// How many instances of @p kind this board wires.
pub fn count(kind: u8) u8 {
    if (!kindValid(kind)) return 0;
    return wiring[kind].count;
}

fn rateFor(ctx: ?*anyopaque, module: Module, out_hz: *u32) callconv(.c) u32 {
    _ = ctx;
    var chip: Module = .{ .kind = Kind.none, .index = 0 };
    const err = toChip(module, &chip);
    if (err != Err.ok) return err;
    return hal.fw_clock_ra8_iface().rate_for(null, chip, out_hz);
}

fn setGate(ctx: ?*anyopaque, module: Module, on: bool) callconv(.c) u32 {
    _ = ctx;
    var chip: Module = .{ .kind = Kind.none, .index = 0 };
    const err = toChip(module, &chip);
    if (err != Err.ok) return err;
    return hal.fw_clock_ra8_iface().set_gate(null, chip, on);
}

/// Whether this board wires @p module.
///
/// Answered from the wiring table alone, so "present" means the board routes
/// it, not that the chip binding happens to have a row. An unwired module is a
/// false answer, not an error: the caller asked a question and got one.
fn hasModule(ctx: ?*anyopaque, module: Module, out_present: *bool) callconv(.c) u32 {
    _ = ctx;
    var chip: Module = .{ .kind = Kind.none, .index = 0 };
    out_present.* = toChip(module, &chip) == Err.ok;
    return Err.ok;
}

/// The profile's ops, filled once at compile time.
const iface: clock.FwClockIface = .{
    .rate_for = rateFor,
    .set_gate = setGate,
    .has_module = hasModule,
};

/// Bind @p clk to this board's profile.
pub fn bind(clk: ?*FwClock) u32 {
    const dst = clk orelse return Err.invalid_arg;
    return hal.fw_clock_bind(dst, &iface, null);
}

/// The board's one handle, bound on first acquisition.
var board_clock: FwClock = .{};

/// The board clock handle.
///
/// Binding cannot fail here: the argument is this file's own storage and a
/// non-null handle with a fully populated ops struct is all `fw_clock_bind`
/// checks. The status is dropped deliberately rather than surfaced, because
/// there is no failure for a caller to handle and a handle-returning accessor
/// that could answer null would make every call site carry a dead branch.
pub fn handle() *const FwClock {
    if (!board_clock.bound) _ = bind(&board_clock);
    return &board_clock;
}
