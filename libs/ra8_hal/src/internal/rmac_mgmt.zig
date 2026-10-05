//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RMAC IRQ status read/clear and statistics snapshot (RA8FW-748), ported
//! from ra8_rmac_mgmt.c. Registers come in through an `mmio` value with
//! read32/write32 so host tests can use a plain buffer. HUM Ch 33.4.

pub const codes = struct {
    pub const ok: u16 = 0x000;
    pub const invalid_arg: u16 = 0x103;
    pub const null_ptr: u16 = 0x504;
};
const err = codes;

pub const port_count: u8 = 2;
pub const pfc_group_count = 2;
pub const pfc_rx_count = 8;

pub const off = struct {
    pub const mpim: usize = 0x008;
    pub const mrmac0: usize = 0x084;
    pub const mrmac1: usize = 0x088;
    pub const meis: usize = 0x200;
    pub const meid: usize = 0x208;
    /// MMISn at 0x210 + 0x10*n, MMIDn at 0x218 + 0x10*n.
    pub const mmis0: usize = 0x210;
    pub const mmid0: usize = 0x218;
    pub const mon_step: usize = 0x10;
    pub const mmpftct: usize = 0x300;
    pub const mapftct: usize = 0x304;
    pub const mpfrct: usize = 0x308;
    pub const mfcict: usize = 0x30C;
    pub const meeect: usize = 0x310;
    pub const mmpcftct: usize = 0x320;
    pub const mapcftct: usize = 0x330;
    pub const mpcfrct: usize = 0x340;
    pub const mrovfc: usize = 0x360;
    pub const mrhcrcec: usize = 0x364;
    /// MRGFCE .. MRXBCPL: 21 consecutive words.
    pub const rx_block: usize = 0x408;
    /// MTGFCE .. MTXBCPL: 10 consecutive words.
    pub const tx_block: usize = 0x508;
};

/// Mirrors ra8_rmac_status_t.
pub const Status = extern struct {
    err_status: u32,
    mon_status: [3]u32,
    phy_monitor: u32,
    mrmac0: u32,
    mrmac1: u32,
};

/// Mirrors ra8_rmac_stats_t: pause/PFC/EEE, then 23 RX words, then 10 TX.
pub const Stats = extern struct {
    pause_tx_manual: u32,
    pause_tx_auto: u32,
    pause_rx: u32,
    false_carrier: u32,
    eee_count: u32,
    pfc_tx_manual: [pfc_group_count]u32,
    pfc_tx_auto: [pfc_group_count]u32,
    pfc_rx: [pfc_rx_count]u32,
    rx_overflow: u32,
    rx_hdr_crc_err: u32,
    rx: [21]u32,
    tx: [10]u32,
};

comptime {
    if (@sizeOf(Status) != 28) @compileError("ra8_rmac_status_t is 28 bytes");
    if (@sizeOf(Stats) != 200) @compileError("ra8_rmac_stats_t is 200 bytes");
    if (@offsetOf(Stats, "rx_overflow") != 68) @compileError("rx_overflow sits at +68");
    if (@offsetOf(Stats, "tx") != 160) @compileError("tx_good_e sits at +160");
}

pub fn getStatus(mmio: anytype, ops: anytype, port: u8, out: ?*Status) u16 {
    const o = out orelse {
        ops.logError("rmac_get_status: out must not be nullptr");
        return err.null_ptr;
    };
    if (port >= port_count) {
        ops.logError("rmac_get_status: port out of range");
        return err.invalid_arg;
    }
    o.err_status = mmio.read32(off.meis);
    for (&o.mon_status, 0..) |*m, i| m.* = mmio.read32(off.mmis0 + i * off.mon_step);
    o.phy_monitor = mmio.read32(off.mpim);
    o.mrmac0 = mmio.read32(off.mrmac0);
    o.mrmac1 = mmio.read32(off.mrmac1);
    return err.ok;
}

/// Writes the disable registers, then the status registers with the masked
/// bits cleared, so a fake without RW1C ends in the hardware's state.
pub fn clearStatus(mmio: anytype, ops: anytype, port: u8, err_mask: u32, mon: [3]u32) u16 {
    if (port >= port_count) {
        ops.logError("rmac_clear_status: port out of range");
        return err.invalid_arg;
    }
    mmio.write32(off.meid, err_mask);
    for (mon, 0..) |m, i| mmio.write32(off.mmid0 + i * off.mon_step, m);
    mmio.write32(off.meis, mmio.read32(off.meis) & ~err_mask);
    for (mon, 0..) |m, i| {
        const at = off.mmis0 + i * off.mon_step;
        mmio.write32(at, mmio.read32(at) & ~m);
    }
    return err.ok;
}

fn readWords(mmio: anytype, base: usize, dst: []u32) void {
    for (dst, 0..) |*d, i| d.* = mmio.read32(base + i * 4);
}

pub fn readStats(mmio: anytype, ops: anytype, port: u8, out: ?*Stats) u16 {
    const o = out orelse {
        ops.logError("read_stats: out must not be nullptr");
        return err.null_ptr;
    };
    if (port >= port_count) {
        ops.logError("read_stats: port out of range");
        return err.invalid_arg;
    }
    o.pause_tx_manual = mmio.read32(off.mmpftct);
    o.pause_tx_auto = mmio.read32(off.mapftct);
    o.pause_rx = mmio.read32(off.mpfrct);
    o.false_carrier = mmio.read32(off.mfcict);
    o.eee_count = mmio.read32(off.meeect);
    readWords(mmio, off.mmpcftct, &o.pfc_tx_manual);
    readWords(mmio, off.mapcftct, &o.pfc_tx_auto);
    readWords(mmio, off.mpcfrct, &o.pfc_rx);
    o.rx_overflow = mmio.read32(off.mrovfc);
    o.rx_hdr_crc_err = mmio.read32(off.mrhcrcec);
    readWords(mmio, off.rx_block, &o.rx);
    readWords(mmio, off.tx_block, &o.tx);
    return err.ok;
}
