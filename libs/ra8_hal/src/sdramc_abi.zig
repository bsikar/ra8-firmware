//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_sdramc.h (RA8FW-608). Pin routing, drive strength and
//! the `.sdram_data` zero-fill stay in C and are called as externs.

const common = @import("abi_common.zig");
const sd = @import("internal/sdramc.zig");

const tag = "SDRAM";
const owner = "ra8_sdramc";
const psel_bus: u8 = 0x0B;
const dscr_high_speed_high: u8 = 2;

extern fn ra8_pfs_route_peripheral(pin: u16, psel: u8, owner: [*:0]const u8) u16;
extern fn ra8_pfs_set_drive_strength(pin: u16, dscr: u8) u16;
extern fn ra8_boot_zero_sdram_bss() u16;

const Hw = struct {
    fn ptr(comptime T: type, addr: usize) *volatile T {
        return @ptrFromInt(addr);
    }
    pub fn read8(_: Hw, off: usize) u8 {
        return ptr(u8, sd.base + off).*;
    }
    pub fn write8(_: Hw, off: usize, value: u8) void {
        ptr(u8, sd.base + off).* = value;
    }
    pub fn write16(_: Hw, off: usize, value: u16) void {
        ptr(u16, sd.base + off).* = value;
    }
    pub fn write32(_: Hw, off: usize, value: u32) void {
        ptr(u32, sd.base + off).* = value;
    }
    pub fn prcr(_: Hw, value: u16) void {
        ptr(u16, sd.prcr_addr).* = value;
    }
    pub fn sdckocr(_: Hw, value: u8) void {
        ptr(u8, sd.sdckocr_addr).* = value;
    }
    pub fn route(_: Hw, pin: u16) u16 {
        return ra8_pfs_route_peripheral(pin, psel_bus, owner);
    }
    pub fn drive(_: Hw, pin: u16) u16 {
        return ra8_pfs_set_drive_strength(pin, dscr_high_speed_high);
    }
    pub fn zeroBss(_: Hw) u16 {
        return ra8_boot_zero_sdram_bss();
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn info(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
};

export fn ra8_sdramc_init() u16 {
    return sd.init(Hw{});
}

export fn ra8_sdramc_deinit() u16 {
    return sd.deinit(Hw{});
}

export fn ra8_sdramc_set_refresh_interval(sdrfcr: u16) u16 {
    return sd.setRefreshInterval(Hw{}, sdrfcr);
}

export fn ra8_sdramc_get_status(out_enabled: ?*u8) u16 {
    return sd.getStatus(Hw{}, out_enabled);
}

export fn ra8_sdramc_enter_stop() u16 {
    return sd.enterStop(Hw{});
}

export fn ra8_sdramc_exit_stop() u16 {
    return sd.exitStop(Hw{});
}

export fn ra8_sdramc_enter_self_refresh() u16 {
    return sd.enterSelfRefresh(Hw{});
}

export fn ra8_sdramc_exit_self_refresh() u16 {
    return sd.exitSelfRefresh(Hw{});
}
