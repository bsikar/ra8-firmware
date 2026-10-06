//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_xspi_init, ra8_xspi_direct_command and ra8_xspi_deinit
//! (RA8FW-868). Logic lives in internal/xspi_init.zig; the callback table is
//! s_xspi_state in xspi_events_abi.zig.

const std = @import("std");
const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ev = @import("internal/xspi_events.zig");
const ini = @import("internal/xspi_init.zig");

const tag = "XSPI";
const sys_base: usize = 0x4001_E000;
const off_prcr: usize = 0x3FA;
const off_ckdivcr: usize = 0x06D;
const off_ckcr: usize = 0x075;

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern var s_xspi_state: [ev.instance_count]ev.State;

const hosted = builtin.os.tag != .freestanding;
const seam = struct {
    extern fn ra8_fake_mmio_wait_eval(reg: *const volatile anyopaque, iter: u32, real_cond: bool) bool;
};

/// Static C flag `s_xspi_clock_inited`: the clock block runs once per boot.
var clock_inited: bool = false;

fn sys8(off: usize) *volatile u8 {
    return @ptrFromInt(sys_base + off);
}

const Regs = struct {
    base: usize,

    pub fn write(self: Regs, off: usize, v: u32) void {
        @as(*volatile u32, @ptrFromInt(self.base + off)).* = v;
    }
    pub fn spin(_: Regs) void {
        var i: u32 = 0;
        while (i < ini.reset_spin) : (i += 1) std.mem.doNotOptimizeAway(i);
    }
};

const Hw = struct {
    pub fn prcr(_: Hw, value: u16) void {
        @as(*volatile u16, @ptrFromInt(sys_base + off_prcr)).* = value;
    }
    pub fn writeDivcr(_: Hw, v: u8) void {
        sys8(off_ckdivcr).* = v;
    }
    pub fn writeCkcr(_: Hw, v: u8) void {
        sys8(off_ckcr).* = v;
    }
    pub fn waitSrdy(_: Hw, set: bool) bool {
        const reg = sys8(off_ckcr);
        var i: u32 = 0;
        while (i < ini.ckcr_spin) : (i += 1) {
            const cond = ((reg.* & ini.srdy) != 0) == set;
            if (if (hosted) seam.ra8_fake_mmio_wait_eval(reg, i, cond) else cond) return true;
        }
        return false;
    }
    pub fn mstpEnable(_: Hw, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn regs(_: Hw, instance: u8) Regs {
        return .{ .base = ev.instanceBase(instance).? };
    }
    pub fn info(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn infoVal(_: Hw, msg: [*:0]const u8, value: u32) void {
        common.ra8_log_emit_info_val(tag, msg, value);
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn fail(_: Hw, msg: [*:0]const u8, code: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", code);
    }
};

export fn ra8_xspi_init(instance: u8, mode: u32) u16 {
    return ini.init(Hw{}, instance, mode, &clock_inited);
}

export fn ra8_xspi_direct_command(instance: u8, cmd_buf: ?[*]const u8, len: u8) u16 {
    const buf = cmd_buf orelse {
        common.ra8_log_emit_error(tag, "cmd_buf must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    if (len > ini.cmd_max_bytes) return common.k_ra8_err_invalid_size;
    const b = ev.instanceBase(instance) orelse return common.k_ra8_err_out_of_range;
    ini.packCommand(Regs{ .base = b }, buf[0..len]);
    return 0;
}

export fn ra8_xspi_deinit(instance: u8) u16 {
    const b = ev.instanceBase(instance) orelse {
        common.ra8_log_emit_error(tag, "instance out of range");
        return common.k_ra8_err_null_ptr;
    };
    ini.deinit(Regs{ .base = b });
    s_xspi_state[instance] = .{};
    return ra8_mstp_disable(ev.mstp_ids[instance]);
}
