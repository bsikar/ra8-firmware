//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI execute-in-place, DTR mode and DQS calibration (RA8FW-866, was
//! part of ra8_xspi.c). Exports live in src/xspi_xip_abi.zig. Mirrors FSP
//! r_ospi_b_xip(true/false) and R_OSPI_B_AutoCalibrate. HUM Ch 44 p 2986.

/// CMCFGCS slot 0: read-command word (+0x04) and address word (+0x0C).
pub const off_cmcfg_read_cmd: usize = 0x14;
pub const off_cmcfg_addr: usize = 0x1C;
pub const off_liocfg0: usize = 0x50;
pub const off_bmctl0: usize = 0x60;
pub const off_cmctlch0: usize = 0x68;
pub const off_cmctlch1: usize = 0x6C;
pub const off_ccctl0: usize = 0x130;

pub const bmctl0_read_only: u32 = 0x55;
pub const bmctl0_read_write: u32 = 0xFF;
pub const xipen: u32 = 1 << 16;
pub const exit_code_shift = 8;
pub const cmd_shift = 16;
pub const ddren: u32 = 1 << 11;
pub const caen: u32 = 1 << 0;
pub const calib_spin: u32 = 1024;

pub const Error = error{ InvalidArg, Timeout };

fn arm(regs: anytype, code: u32) void {
    regs.write(off_bmctl0, bmctl0_read_only);
    regs.write(off_cmctlch0, code | xipen);
    regs.write(off_cmctlch1, code | xipen);
}

pub fn enter(regs: anytype, enter_code: u8, exit_code: u8) void {
    arm(regs, @as(u32, enter_code) | @as(u32, exit_code) << exit_code_shift);
}

pub fn exit(regs: anytype) void {
    regs.write(off_cmctlch0, 0);
    regs.write(off_cmctlch1, 0);
    regs.write(off_bmctl0, bmctl0_read_write);
}

pub fn setMode(regs: anytype, enable: bool, read_cmd: u8, addr_bytes: u8) Error!void {
    if (addr_bytes != 3 and addr_bytes != 4) return error.InvalidArg;
    regs.write(off_cmcfg_read_cmd, @as(u32, read_cmd) << cmd_shift);
    regs.write(off_cmcfg_addr, addr_bytes);
    if (enable) arm(regs, 0) else exit(regs);
}

pub fn setDtr(regs: anytype, enable: bool) void {
    const v = regs.read(off_liocfg0);
    regs.write(off_liocfg0, if (enable) v | ddren else v & ~ddren);
}

/// Arm CAEN and wait for the controller to clear it. Only the real
/// controller clears the bit; hosted builds answer through `eval`.
pub fn calibrate(regs: anytype) Error!void {
    regs.write(off_ccctl0, regs.read(off_ccctl0) | caen);
    var i: u32 = 0;
    while (i < calib_spin) : (i += 1) {
        if (regs.eval(off_ccctl0, i, regs.read(off_ccctl0) & caen == 0)) return;
    }
    return error.Timeout;
}
