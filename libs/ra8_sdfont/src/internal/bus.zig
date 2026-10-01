//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Getting the card onto the bus: pin routing, the SCI Simple-SPI controller,
//! the three-callback transport the SD driver wants, and the FAT mount.
//!
//! Every line here is a call into a lower ring, so there is nothing to test on
//! the host. The decisions live in `policy.zig`.

const shared = @import("root.zig");

pub const err = shared.err;

const Level = enum(u8) { low = 0, high = 1 };
const Psel = enum(u8) { sci_async = 0x04 };
const SpiMode = enum(u8) { mode_0 = 0 };

/// Mandatory power-on clock floor, `k_ra8_sdmmc_spi_clock_init_hz`.
const spi_clock_init_hz: u32 = 400_000;

/// `ra8_sci_spi_cfg_t`.
const SciSpiCfg = extern struct {
    baud_hz: u32,
    pclk_hz: u32,
    mode: SpiMode,
    lsb_first: bool,
};

/// `ra8_sdmmc_spi_transport_t`: the driver-to-bus seam, all three callbacks
/// non-NULL.
const Transport = extern struct {
    set_clock: *const fn (ctx: ?*anyopaque, hz: u32) callconv(.c) u16,
    cs: *const fn (ctx: ?*anyopaque, asserted: bool) callconv(.c) u16,
    xfer: *const fn (ctx: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u32) callconv(.c) u16,
    ctx: ?*anyopaque,
};

extern fn ra8_pfs_route_peripheral(pin: u16, psel: Psel, owner: [*:0]const u8) u16;

/// `ra8_pin_interface_t` (libs/ra8_core/inc/ra8_pin_interface.h). The whole
/// vtable, in declaration order, because the layout has to be exact: this
/// struct is read through a pointer the C side owns.
const PinInterface = extern struct {
    output_init: *const fn (ctx: ?*anyopaque, pin: u16, init_level: Level) callconv(.c) u16,
    input_init: *const fn (ctx: ?*anyopaque, pin: u16, pull: u8) callconv(.c) u16,
    write: *const fn (ctx: ?*anyopaque, pin: u16, level: Level) callconv(.c) u16,
    read: *const fn (ctx: ?*anyopaque, pin: u16, out_level: ?*Level) callconv(.c) u16,
    toggle: *const fn (ctx: ?*anyopaque, pin: u16) callconv(.c) u16,
    release: *const fn (ctx: ?*anyopaque, pin: u16) callconv(.c) u16,
    ctx: ?*anyopaque,
};

extern fn ra8_pin_interface_default() *const PinInterface;
extern fn ra8_sci_spi_init(channel: u8, cfg: *const SciSpiCfg) u16;
extern fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) u16;
extern fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) u16;
extern fn ra8_sdmmc_spi_init(transport: *const Transport) u16;
extern fn ra8_sdmmc_spi_bind_fs_backend(out_backend: *FsBackend) u16;

/// `ra8_fs_backend_t`: block-device vtable plus its context. Filled by the SD
/// driver and handed straight to the mount, never read here, but the layout
/// has to be exact because the driver writes through this pointer. Mirrors
/// `libs/ra8_sdmmc_spi/src/internal/root.zig`, which is the same struct on the
/// producing side.
const FsBackend = extern struct {
    read_block: ?*const fn (?*anyopaque, u64, u32, ?[*]u8) callconv(.c) u16 = null,
    write_block: ?*const fn (?*anyopaque, u64, u32, ?[*]const u8) callconv(.c) u16 = null,
    get_capacity: ?*const fn (?*anyopaque, ?*u64, ?*u32) callconv(.c) u16 = null,
    erase_blocks: ?*const fn (?*anyopaque, u64, u64) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

extern fn ra8_fs_mount(backend: *const FsBackend, out_handle: *?*anyopaque) u16;
extern fn ra8_fs_unmount(handle: *anyopaque) u16;

/// What the transport callbacks need, carried through `ctx`. One card at a
/// time, matching the single static context the C kept.
const Context = struct {
    channel: u8 = 0,
    pclka_hz: u32 = 0,
    cs: u16 = 0,
};

var context: Context = .{};

fn setClock(ctx: ?*anyopaque, hz: u32) callconv(.c) u16 {
    const self: *const Context = @ptrCast(@alignCast(ctx orelse return err.null_ptr));
    return ra8_sci_spi_set_clock(self.channel, hz, self.pclka_hz);
}

fn chipSelect(ctx: ?*anyopaque, asserted: bool) callconv(.c) u16 {
    const self: *const Context = @ptrCast(@alignCast(ctx orelse return err.null_ptr));
    const pins = ra8_pin_interface_default();
    return pins.write(pins.ctx, self.cs, if (asserted) .low else .high);
}

fn transfer(ctx: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u32) callconv(.c) u16 {
    const self: *const Context = @ptrCast(@alignCast(ctx orelse return err.null_ptr));
    return ra8_sci_spi_xfer(self.channel, tx, rx, len);
}

/// Pins the card needs, in the order the C routed them.
pub const Pins = struct {
    sck: u16,
    cipo: u16,
    copi: u16,
    cs: u16,
};

/// Route the three SCI pins, park CS high, and bring the controller up at the
/// mandatory init clock. The card is not addressed yet.
pub fn bringUpSpi(channel: u8, pclka_hz: u32, pins: Pins) u16 {
    context = .{ .channel = channel, .pclka_hz = pclka_hz, .cs = pins.cs };

    const routes = [_]struct { pin: u16, owner: [*:0]const u8 }{
        .{ .pin = pins.sck, .owner = "sdfont.sck" },
        .{ .pin = pins.cipo, .owner = "sdfont.cipo" },
        .{ .pin = pins.copi, .owner = "sdfont.copi" },
    };
    for (routes) |route| {
        const status = ra8_pfs_route_peripheral(route.pin, .sci_async, route.owner);
        if (status != err.ok) {
            return status;
        }
    }

    const pin_if = ra8_pin_interface_default();
    const parked = pin_if.output_init(pin_if.ctx, pins.cs, .high);
    if (parked != err.ok) {
        return parked;
    }

    const cfg: SciSpiCfg = .{
        .baud_hz = spi_clock_init_hz,
        .pclk_hz = pclka_hz,
        .mode = .mode_0,
        .lsb_first = false,
    };
    return ra8_sci_spi_init(channel, &cfg);
}

/// Hand the driver its transport, then mount the FAT volume it exposes.
pub fn mount(out_handle: *?*anyopaque) u16 {
    const transport: Transport = .{
        .set_clock = setClock,
        .cs = chipSelect,
        .xfer = transfer,
        .ctx = &context,
    };

    const started = ra8_sdmmc_spi_init(&transport);
    if (started != err.ok) {
        return started;
    }

    var backend: FsBackend = .{};
    const bound = ra8_sdmmc_spi_bind_fs_backend(&backend);
    if (bound != err.ok) {
        return bound;
    }

    return ra8_fs_mount(&backend, out_handle);
}

/// Release the volume. The result is deliberately dropped: the font is already
/// in caller storage and a failed unmount must not mask that.
pub fn unmount(handle: *anyopaque) void {
    _ = ra8_fs_unmount(handle);
}

comptime {
    const std = @import("std");
    const ptr = @sizeOf(usize);

    // Written through by the SD driver, so a mismatch here is stack
    // corruption rather than a wrong value.
    std.debug.assert(@sizeOf(FsBackend) == 5 * ptr);
    std.debug.assert(@offsetOf(FsBackend, "ctx") == 4 * ptr);

    // Read by the SD driver through the pointer we hand it.
    std.debug.assert(@sizeOf(Transport) == 4 * ptr);
    std.debug.assert(@offsetOf(Transport, "ctx") == 3 * ptr);

    // Passed by value into ra8_sci_spi_init.
    std.debug.assert(@sizeOf(SciSpiCfg) == 12);
    std.debug.assert(@offsetOf(SciSpiCfg, "mode") == 8);
    std.debug.assert(@offsetOf(SciSpiCfg, "lsb_first") == 9);
}
