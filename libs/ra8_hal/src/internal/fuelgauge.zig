//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MAX17048 fuel gauge over the injected I2C transfer seam
//! (inc/ra8_fuelgauge.h, RA8FW-553). Register map from
//! inc/ra8_fuelgauge_max17048_regs.h.

/// ra8_err_t values this driver returns. Non-exhaustive so a bus
/// callback's own code passes through unchanged.
pub const Status = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    not_initialized = 0x10F,
    hw_not_ready = 0x202,
    null_ptr = 0x504,
    _,
};

pub const reg_vcell: u8 = 0x02;
pub const reg_soc: u8 = 0x04;
pub const reg_version: u8 = 0x08;
pub const reg_crate: u8 = 0x16;
pub const default_addr_7b: u8 = 0x36;

const probe_dead: u16 = 0x0000;
const probe_float: u16 = 0xFFFF;

/// `ra8_i2c_bus_ops_t`.
pub const BusOps = extern struct {
    write: ?*const fn (?*anyopaque, u8, [*]const u8, u32, bool) callconv(.C) u16 = null,
    read: ?*const fn (?*anyopaque, u8, [*]u8, u32) callconv(.C) u16 = null,
    transfer: ?*const fn (?*anyopaque, u8, [*]const u8, u32, [*]u8, u32) callconv(.C) u16 = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_fuelgauge_cfg_t`.
pub const Cfg = extern struct { bus: BusOps, target_7b: u8 };

/// `ra8_fuelgauge_state_t`.
pub const State = extern struct { vcell_mv: u16, crate_raw: i16, soc_pct: u8, charging: bool };

/// `ra8_fuelgauge_t`.
pub const Handle = extern struct { bus: BusOps = .{}, target_7b: u8 = 0, opened: bool = false };

comptime {
    const ptr = @sizeOf(usize);
    if (@sizeOf(BusOps) != 4 * ptr) @compileError("BusOps layout");
    if (@sizeOf(State) != 6) @compileError("State layout");
    if (@offsetOf(Handle, "opened") != 4 * ptr + 1) @compileError("Handle layout");
}

/// VCELL LSB is 78.125 uV, so mV = raw * 5 / 64.
pub fn vcellMv(raw: u16) u16 {
    return @intCast(@as(u32, raw) * 5 / 64);
}

/// SOC's high byte is the integer percent.
pub fn socPct(raw: u16) u8 {
    return @truncate(raw >> 8);
}

pub fn decode(vcell: u16, soc: u16, crate: u16) State {
    const signed: i16 = @bitCast(crate);
    return .{ .vcell_mv = vcellMv(vcell), .crate_raw = signed, .soc_pct = socPct(soc), .charging = signed >= 0 };
}

/// One big-endian 16-bit register read through `transfer`.
fn readReg16(bus: *const BusOps, addr: u8, reg: u8, out: *u16) Status {
    var ptr = [1]u8{reg};
    var raw = [2]u8{ 0, 0 };
    const code: Status = @enumFromInt(bus.transfer.?(bus.ctx, addr, &ptr, 1, &raw, 2));
    if (code != .ok) return code;
    out.* = (@as(u16, raw[0]) << 8) | raw[1];
    return .ok;
}

/// Probe VERSION and latch the seam (ra8_fuelgauge_open after null checks).
pub fn open(fg: *Handle, cfg: *const Cfg) Status {
    if (cfg.bus.transfer == null) return .invalid_arg;
    var version: u16 = 0;
    const code = readReg16(&cfg.bus, cfg.target_7b, reg_version, &version);
    if (code != .ok) return code;
    if (version == probe_dead or version == probe_float) return .hw_not_ready;
    fg.* = .{ .bus = cfg.bus, .target_7b = cfg.target_7b, .opened = true };
    return .ok;
}

/// VCELL, SOC and CRATE, decoded (ra8_fuelgauge_read after null checks).
pub fn read(fg: *const Handle, out: *State) Status {
    if (!fg.opened) return .not_initialized;
    var raw = [3]u16{ 0, 0, 0 };
    for ([_]u8{ reg_vcell, reg_soc, reg_crate }, &raw) |reg, *slot| {
        const code = readReg16(&fg.bus, fg.target_7b, reg, slot);
        if (code != .ok) return code;
    }
    out.* = decode(raw[0], raw[1], raw[2]);
    return .ok;
}

/// Zero the handle (ra8_fuelgauge_close after null checks).
pub fn close(fg: *Handle) Status {
    if (!fg.opened) return .not_initialized;
    fg.* = .{};
    return .ok;
}
