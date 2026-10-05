//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_gpio_write/toggle/read and ra8_pfs_set_drive_strength
//! (RA8FW-763), moved out of gpio.c. Logic is in internal/gpio_pins.zig.
//! Claim/init, peripheral routing, IRQ attach and the pin interface stay
//! in gpio.c and call these symbols.

const common = @import("abi_common.zig");
const gp = @import("internal/gpio_pins.zig");

const tag = "GPIO";
const ok = common.k_ra8_ok;
const err_invalid_port: u16 = 0x206;
const err_invalid_pin: u16 = 0x207;
const level_low: u8 = 0;
const level_high: u8 = 1;

const Hw = struct {
    pub fn read32(_: Hw, addr: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    pub fn write8(_: Hw, addr: usize, value: u8) void {
        @as(*volatile u8, @ptrFromInt(addr)).* = value;
    }
};

fn errOf(s: gp.Status) u16 {
    return switch (s) {
        .ok => ok,
        .invalid_port => err_invalid_port,
        .invalid_pin => err_invalid_pin,
    };
}

export fn ra8_gpio_write(pin: u16, level: u8) u16 {
    const p = switch (gp.decode(pin)) {
        .pin => |v| v,
        .err => |e| return errOf(e),
    };
    const hw = Hw{};
    hw.write32(gp.portReg(p.port, gp.off_pcntr3), gp.writeValue(p, level == level_high));
    return ok;
}

export fn ra8_gpio_toggle(pin: u16) u16 {
    const p = switch (gp.decode(pin)) {
        .pin => |v| v,
        .err => |e| return errOf(e),
    };
    const hw = Hw{};
    const pcntr1 = hw.read32(gp.portReg(p.port, gp.off_pcntr1));
    hw.write32(gp.portReg(p.port, gp.off_pcntr3), gp.toggleValue(p, pcntr1));
    return ok;
}

export fn ra8_gpio_read(pin: u16, out_level: ?*u8) u16 {
    const out = out_level orelse {
        common.ra8_log_emit_error(tag, "out_level must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    const p = switch (gp.decode(pin)) {
        .pin => |v| v,
        .err => |e| return errOf(e),
    };
    const hw = Hw{};
    out.* = if (gp.levelOf(p, hw.read32(gp.portReg(p.port, gp.off_pcntr2)))) level_high else level_low;
    return ok;
}

/// HUM 20.2.4: a DSCR-only update does not retouch PSEL, so no PMR dance.
export fn ra8_pfs_set_drive_strength(pin: u16, dscr: u8) u16 {
    const p = switch (gp.decode(pin)) {
        .pin => |v| v,
        .err => |e| return errOf(e),
    };
    const hw = Hw{};
    const addr = gp.pfsAddr(p);
    gp.unlock(hw);
    hw.write32(addr, gp.withDscr(hw.read32(addr), dscr));
    gp.lock(hw);
    common.ra8_log_emit_info_val(tag, "pin drive strength", pin);
    return ok;
}
