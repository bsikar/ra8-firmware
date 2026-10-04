//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C block in legacy-I2C responder mode (HUM Ch 40.2). Port of
//! ra8_i3c_i2c_peripheral.c (RA8FW-557). Module-stop and logging stay in
//! the ABI file; this file only touches the R_I3C registers.

/// R_I3C0 (`k_ra8_i3c_i2c0_base_addr`).
pub const base: usize = 0x4035F000;
/// `k_ra8_i3c_i2c_channel_count`.
pub const channel_count: u8 = 1;

pub const off_bctl: usize = 0x014;
pub const off_msdvad: usize = 0x018;
pub const off_rstctl: usize = 0x020;
pub const off_svctl: usize = 0x064;
pub const off_ntdtbp0: usize = 0x158;
pub const off_bst: usize = 0x1D0;
pub const off_ntst: usize = 0x1E0;
/// Bytes a host test buffer must cover.
pub const window_len: usize = off_ntst + 4;

/// Bounded poll iterations per byte.
pub const spin_budget: u32 = 50_000;
/// 7-bit address ceiling.
pub const max_addr_7b: u8 = 0x7F;

pub const ntst_tdbef0: u32 = 1 << 0;
pub const ntst_rdbff0: u32 = 1 << 1;
pub const svctl_gcae: u32 = 1 << 0;
pub const bctl_buse: u32 = 1 << 31;
pub const bst_spcnddf: u32 = 1 << 1;
pub const bst_nackdf: u32 = 1 << 4;

/// `ra8_i3c_i2c_peripheral_status_*` bits.
pub const Status = struct {
    pub const aas: u8 = 0x01;
    pub const rx_full: u8 = 0x02;
    pub const tx_empty: u8 = 0x04;
    pub const stop: u8 = 0x08;
    pub const nack: u8 = 0x10;
};

/// `ra8_i3c_i2c_peripheral_cfg_t`.
pub const Cfg = extern struct {
    peripheral_addr_7b: u8,
    general_call: u8,
};

comptime {
    if (@sizeOf(Cfg) != 2) @compileError("ra8_i3c_i2c_peripheral_cfg_t is 2 bytes");
}

pub const Error = error{Timeout};

/// One R_I3C channel, by base address so host tests can use a buffer.
pub const Block = struct {
    base: usize,

    pub fn reg(block: Block, off: usize) *volatile u32 {
        return @ptrFromInt(block.base + off);
    }
};

/// `i3c_i2c_regs(channel)`: null past the last channel.
pub fn regsFor(channel: u8) ?Block {
    if (channel >= channel_count) return null;
    return .{ .base = base };
}

/// Program the responder address and enable the bus (MSTP already open).
pub fn configure(block: Block, cfg: Cfg) void {
    block.reg(off_rstctl).* = 0;
    block.reg(off_msdvad).* = @as(u32, cfg.peripheral_addr_7b) << 1;
    block.reg(off_svctl).* = if (cfg.general_call != 0) svctl_gcae else 0;
    block.reg(off_bctl).* = bctl_buse;
}

/// Disable the bus and forget the responder address.
pub fn clear(block: Block) void {
    block.reg(off_bctl).* = 0;
    block.reg(off_msdvad).* = 0;
    block.reg(off_svctl).* = 0;
}

/// Poll NTST until every bit of `mask` is set, at most `budget` reads.
pub fn waitNtst(block: Block, mask: u32, budget: u32) bool {
    var i: u32 = 0;
    while (i < budget) : (i += 1) {
        if ((block.reg(off_ntst).* & mask) == mask) return true;
    }
    return false;
}

/// Write each byte once the transmit buffer reports empty.
pub fn send(block: Block, data: []const u8, budget: u32) Error!void {
    for (data) |byte| {
        if (!waitNtst(block, ntst_tdbef0, budget)) return error.Timeout;
        block.reg(off_ntdtbp0).* = byte;
    }
}

/// Read each byte once the receive buffer reports full.
pub fn receive(block: Block, buf: []u8, budget: u32) Error!void {
    for (buf) |*byte| {
        if (!waitNtst(block, ntst_rdbff0, budget)) return error.Timeout;
        byte.* = @truncate(block.reg(off_ntdtbp0).*);
    }
}

/// Map NTST, BST and MSDVAD onto the status bits.
pub fn statusMask(block: Block) u8 {
    const ntst = block.reg(off_ntst).*;
    const bst = block.reg(off_bst).*;
    var mask: u8 = 0;
    if ((ntst & ntst_rdbff0) != 0) mask |= Status.rx_full;
    if ((ntst & ntst_tdbef0) != 0) mask |= Status.tx_empty;
    if ((bst & bst_spcnddf) != 0) mask |= Status.stop;
    if ((bst & bst_nackdf) != 0) mask |= Status.nack;
    if (block.reg(off_msdvad).* != 0) mask |= Status.aas;
    return mask;
}
