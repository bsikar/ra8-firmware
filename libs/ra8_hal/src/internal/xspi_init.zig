//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! XSPI block clock, ra8_xspi_init, ra8_xspi_direct_command and
//! ra8_xspi_deinit (RA8FW-868, the last of ra8_xspi.c). Exports live in
//! src/xspi_init_abi.zig. HUM Ch 9.2.40 / 9.2.45 (OCTACKDIVCR, OCTACKCR),
//! Ch 11.2.7 (MSTPCRB), Ch 44 p 2986.

const ev = @import("xspi_events.zig");

/// `k_ra8_xspi_ckcr_spin`, `k_ra8_xspi_reset_spin`.
pub const ckcr_spin: u32 = 262144;
pub const reset_spin: u32 = 100000;
/// OCTACKCR: source MOCO, SREQ bit 6, SRDY bit 7.
pub const src_moco: u8 = 0x01;
pub const sreq: u8 = 1 << 6;
pub const srdy: u8 = 1 << 7;
pub const prcr_unlock_cgc: u16 = 0xA501;
pub const prcr_lock_all: u16 = 0xA500;

pub const off_wrapcfg: usize = 0x000;
pub const off_comcfg: usize = 0x004;
pub const off_liocfg0: usize = 0x050;
pub const off_liocfg1: usize = 0x054;
pub const off_bmctl0: usize = 0x060;
pub const off_cmctlch0: usize = 0x068;
pub const off_cmctlch1: usize = 0x06C;
pub const off_cdctl0: usize = 0x070;
pub const off_cdbuf: usize = 0x080;
pub const off_lioctl: usize = 0x108;
pub const off_inte: usize = 0x198;

/// IS25LX512M sits on controller CS1; CDCTL0.CSSEL is bit 3.
pub const onboard_cs: u32 = 1;
pub const cdctl0_cssel: u32 = (onboard_cs << 3) & (1 << 3);
pub const lioctl_wpcs: u32 = 1 << 0;
pub const lioctl_rstcs: u32 = 1 << 16;
/// A CDBUF slot holds 16 bytes.
pub const cmd_max_bytes: u8 = 16;

pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

fn clockHandshake(hw: anytype) u16 {
    hw.writeDivcr(0);
    hw.writeCkcr(src_moco | sreq);
    if (!hw.waitSrdy(true)) {
        hw.err("xspi: OCTACKSRDY=1 timeout");
        return hw_timeout;
    }
    hw.writeCkcr(src_moco);
    if (!hw.waitSrdy(false)) {
        hw.err("xspi: OCTACKSRDY=0 timeout");
        return hw_timeout;
    }
    return 0;
}

/// Switch OCTACLK to MOCO /1 once per boot. PRCR is re-locked on every
/// path (the C left it unlocked after an SRDY timeout).
pub fn clockInit(hw: anytype, done: *bool) u16 {
    if (done.*) return 0;
    hw.prcr(prcr_unlock_cgc);
    const rc = clockHandshake(hw);
    hw.prcr(prcr_lock_all);
    if (rc != 0) return rc;
    done.* = true;
    hw.info("octa block clock stable");
    return 0;
}

pub fn applyConfig(regs: anytype, mode: u32) void {
    regs.write(off_bmctl0, 0);
    regs.write(off_cmctlch0, 0);
    regs.write(off_cmctlch1, 0);
    regs.write(off_wrapcfg, 0);
    regs.write(off_comcfg, 0);
    regs.write(off_liocfg1, mode);
    regs.write(off_cdctl0, cdctl0_cssel);
    regs.write(ev.off_intc, ev.ints_mask_all);
}

/// WPCS stays high (write-protect deasserted); only RSTCS toggles.
pub fn resetDevice(regs: anytype) void {
    regs.write(off_lioctl, lioctl_wpcs);
    regs.spin();
    regs.write(off_lioctl, lioctl_wpcs | lioctl_rstcs);
    regs.spin();
}

/// Clock first: MSTPB16/17 must be written after OCTACLK is stable.
pub fn init(hw: anytype, instance: u8, mode: u32, done: *bool) u16 {
    if (!ev.inRange(instance)) {
        hw.err("instance out of range");
        return null_ptr;
    }
    const clk = clockInit(hw, done);
    if (clk != 0) return clk;
    const mst = hw.mstpEnable(ev.mstp_ids[instance]);
    if (mst != 0) {
        hw.fail("xspi_init: mstp enable", mst);
        return mst;
    }
    const regs = hw.regs(instance);
    applyConfig(regs, mode);
    resetDevice(regs);
    hw.infoVal("xspi_init inst", instance);
    return 0;
}

/// Little-endian pack into CDBUF[0..3], one store per word, including a
/// final partial word.
pub fn packCommand(regs: anytype, bytes: []const u8) void {
    var word: u32 = 0;
    for (bytes, 0..) |b, i| {
        word |= @as(u32, b) << @intCast((i % 4) * 8);
        if (i % 4 == 3) {
            regs.write(off_cdbuf + (i / 4) * 4, word);
            word = 0;
        }
    }
    if (bytes.len % 4 != 0) regs.write(off_cdbuf + (bytes.len / 4) * 4, word);
}

pub fn deinit(regs: anytype) void {
    regs.write(off_liocfg0, 0);
    regs.write(off_inte, 0);
    regs.write(ev.off_intc, ev.ints_mask_all);
}
