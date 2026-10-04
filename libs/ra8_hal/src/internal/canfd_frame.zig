//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CAN-FD frame data path (RA8FW-587, was ra8_canfd_frame.c): transmit on
//! TX message buffer 0, poll-receive from RX FIFO 0, and the TEC/REC error
//! counters. Pure: one channel's registers come in through a `regs` value
//! with read8/write8/read32/write32 at an offset from the channel base.
//! HUM Ch 41 p 2754..2810; FSP r_canfd.c ~668..724.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const no_data: u16 = 0x10A;

/// Offsets from the channel base (offsetof against ra8_canfd_regs.h).
pub const off_sts: usize = 0x008;
pub const off_rfsts0: usize = 0x044;
pub const off_rfpctr0: usize = 0x04C;
pub const off_tmc0: usize = 0x070;
pub const off_tmsts0: usize = 0x074;
pub const off_rf0: usize = 0x520;
pub const off_tm0: usize = 0x604;
/// ID, PTR and FDCTR/FDSTS words, then the 64-byte data field.
pub const off_id: usize = 0x00;
pub const off_ptr: usize = 0x04;
pub const off_fd: usize = 0x08;
pub const off_df: usize = 0x0C;

pub const data_bytes: usize = 64;
pub const dlc_max: u8 = 15;
pub const id_std_mask: u32 = 0x0000_07FF;
pub const id_ext_mask: u32 = 0x1FFF_FFFF;
pub const id_ide: u32 = 1 << 31;
pub const fd_brs: u32 = 1 << 1;
pub const fd_fdf: u32 = 1 << 2;
pub const ptr_shift_dlc: u5 = 28;
pub const ptr_mask_dlc: u32 = 0xF;
pub const tmc_txreq: u8 = 1;
/// TMTRF[1]: set only once the frame is on the wire and the MB is free.
pub const tmtrf_done: u8 = 0x04;
pub const rfsts_empty: u32 = 1;
pub const rfpctr_ack: u32 = 0xFF;
/// TX-completion poll budget: about 500 us at 1 GHz, 5 cycles a poll.
pub const tx_spin: u32 = 100_000;

/// `ra8_canfd_frame_t` (inc/ra8_canfd.h), 72 bytes.
pub const Frame = extern struct {
    id: u32,
    dlc: u8,
    is_extended: u8,
    is_fd: u8,
    is_brs: u8,
    data: [data_bytes]u8,
};

pub fn validate(f: *const Frame) u16 {
    if (f.dlc > dlc_max) return invalid_arg;
    const mask = if (f.is_extended == 0) id_std_mask else id_ext_mask;
    if (f.id & ~mask != 0) return invalid_arg;
    if (f.is_brs != 0 and f.is_fd == 0) return invalid_arg;
    return ok;
}

pub fn txId(f: *const Frame) u32 {
    if (f.is_extended != 0) return (f.id & id_ext_mask) | id_ide;
    return f.id & id_std_mask;
}

pub fn txFdctr(f: *const Frame) u32 {
    var w: u32 = 0;
    if (f.is_fd != 0) w |= fd_fdf;
    if (f.is_brs != 0) w |= fd_brs;
    return w;
}

/// Best-effort bounded spin on TMTRF; no error when the budget runs out.
/// Returns the number of polls issued.
pub fn waitTxComplete(regs: anytype) u32 {
    var i: u32 = 0;
    while (i < tx_spin) : (i += 1) {
        if (regs.read8(off_tmsts0) & tmtrf_done != 0) return i + 1;
    }
    return tx_spin;
}

/// Validated frame to TX MB 0. TMTRF is cleared first: TXREQ is ignored
/// while it still reads "transmission successful" (HUM p ~2756).
pub fn transmit(regs: anytype, f: *const Frame) u16 {
    const v = validate(f);
    if (v != ok) return v;
    regs.write8(off_tmsts0, 0);
    regs.write32(off_tm0 + off_id, txId(f));
    regs.write32(off_tm0 + off_ptr, (@as(u32, f.dlc) & ptr_mask_dlc) << ptr_shift_dlc);
    regs.write32(off_tm0 + off_fd, txFdctr(f));
    for (f.data, 0..) |b, i| regs.write8(off_tm0 + off_df + i, b);
    regs.write8(off_tmc0, tmc_txreq);
    _ = waitTxComplete(regs);
    return ok;
}

pub fn decodeHeader(id_word: u32, ptr_word: u32, fdsts: u32, out: *Frame) void {
    const ext = id_word & id_ide != 0;
    out.is_extended = @intFromBool(ext);
    out.id = id_word & (if (ext) id_ext_mask else id_std_mask);
    out.dlc = @intCast((ptr_word >> ptr_shift_dlc) & ptr_mask_dlc);
    out.is_fd = @intFromBool(fdsts & fd_fdf != 0);
    out.is_brs = @intFromBool(fdsts & fd_brs != 0);
}

/// One frame from RX FIFO 0, then the 0xFF dummy write that pops it.
pub fn receive(regs: anytype, out: *Frame) u16 {
    if (regs.read32(off_rfsts0) & rfsts_empty != 0) return no_data;
    const id_word = regs.read32(off_rf0 + off_id);
    const ptr_word = regs.read32(off_rf0 + off_ptr);
    const fdsts = regs.read32(off_rf0 + off_fd);
    decodeHeader(id_word, ptr_word, fdsts, out);
    for (&out.data, 0..) |*b, i| b.* = regs.read8(off_rf0 + off_df + i);
    regs.write32(off_rfpctr0, rfpctr_ack);
    return ok;
}

pub const Counters = struct { tec: u8, rec: u8 };

/// TEC[31:24] and REC[23:16] live in CFDC0.STS (HUM p 2766).
pub fn errorCounters(regs: anytype) Counters {
    const sts = regs.read32(off_sts);
    return .{ .tec = @truncate(sts >> 24), .rec = @truncate(sts >> 16) };
}
