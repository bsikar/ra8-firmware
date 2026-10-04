//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA time-aware shaper flows (RA8FW-590, was ra8_etha_tas.c). Pure: one
//! port's ETHA block comes in through a `regs` value (read32/write32 by
//! offset) and logging through an `ops` value. Implements three HUM flows
//! verbatim: TAS RAM reset (Figure 32.8 p 1667), TAS setting with the
//! per-entry learn (Figures 32.11 / 32.14 p 1675-1677) and TAS entry read
//! (Figure 32.15 p 1678).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const busy: u16 = 0x109;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

pub const port_bases = [_]usize{ 0x403C_A000, 0x403C_C000 };

pub const off_tasc: usize = 0x300;
pub const off_igsc: usize = 0x304;
pub const off_enc: usize = 0x320; // EATASENC[8], stride 4
pub const off_cstc0: usize = 0x3A0;
pub const off_cstc1: usize = 0x3A4;
pub const off_ctc: usize = 0x3B0;
pub const off_gl0: usize = 0x3C0;
pub const off_gl1: usize = 0x3C4;
pub const off_glr: usize = 0x3C8;
pub const off_gr: usize = 0x3D0;
pub const off_grr: usize = 0x3D4;
pub const off_rirm: usize = 0x3E4;

pub const tc_count = 8;
pub const entries_max: u32 = 119;
pub const spins: u32 = 100_000;
pub const mask_gtl: u32 = 0x0FFF_FFFF;
pub const mask_gal: u32 = 0xFF;
pub const mask_aen: u32 = 0x1FF;
const gate_state_bit: u32 = 1 << 28; // EATASGL1.TASGSL / EATASGRR.TASGSR
const busy_bit: u32 = 1 << 31; // EATASGLR.GL / EATASGRR.GR
const rirm_start: u32 = 1 << 0; // EATASRIRM.TASRIOG
const rirm_ready: u32 = 1 << 1; // EATASRIRM.TASRR
const tasc_tase: u32 = 1 << 0;
const tasc_tascc: u32 = 1 << 1;
const tasc_tasci: u32 = 1 << 2;

/// `ra8_etha_tas_entry_t`.
pub const Entry = extern struct { gate_time_ns: u32, gate_open: bool };
/// `ra8_etha_tas_queue_t`.
pub const Queue = extern struct { entries: ?[*]const Entry, count: u16 };
pub const Queues = [tc_count]Queue;

pub fn portBase(port: u8) ?usize {
    return if (port < port_bases.len) port_bases[port] else null;
}

/// ra8_hw_wait_flag_set32 / clear32: `budget` reads, then hw_timeout.
pub fn wait(regs: anytype, off: usize, mask: u32, want_set: bool, budget: u32) u16 {
    var i: u32 = 0;
    while (i < budget) : (i += 1) {
        if ((regs.read32(off) & mask != 0) == want_set) return ok;
    }
    return hw_timeout;
}

/// Figure 32.14: address, then GL1 (which raises GL), then poll GL clear.
fn learnEntry(regs: anytype, ops: anytype, address: u32, entry: *const Entry) u16 {
    regs.write32(off_gl0, address & mask_gal);
    var gl1 = entry.gate_time_ns & mask_gtl;
    if (entry.gate_open) gl1 |= gate_state_bit;
    regs.write32(off_gl1, gl1);
    const e = wait(regs, off_glr, busy_bit, false, spins);
    if (e != ok) ops.logError("etha_tas: EATASGLR.GL never cleared");
    return e;
}

/// Rejects the whole schedule before any write: per-queue entry pointers,
/// TASGTL[27:0] gate times and the TAS RAM capacity (HUM p 1647).
pub fn validate(ops: anytype, queues: *const Queues) u16 {
    var total: u32 = 0;
    for (queues) |q| {
        if (q.count == 0) continue;
        const entries = q.entries orelse {
            ops.logError("etha_tas: queue entries null");
            return null_ptr;
        };
        for (entries[0..q.count]) |en| {
            if (en.gate_time_ns > mask_gtl) {
                ops.logError("etha_tas: gate time exceeds TASGTL[27:0]");
                return invalid_arg;
            }
        }
        total += q.count;
    }
    if (total > entries_max) {
        ops.logError("etha_tas: total entries exceed TAS RAM capacity");
        return invalid_arg;
    }
    return ok;
}

/// Queues' blocks sit back to back from the hardware's TASCA base.
fn learnAll(regs: anytype, ops: anytype, queues: *const Queues, base: u32) u16 {
    var address = base;
    for (queues) |q| {
        const entries = q.entries orelse continue;
        for (entries[0..q.count]) |*en| {
            const e = learnEntry(regs, ops, address, en);
            if (e != ok) return e;
            address += 1;
        }
    }
    return ok;
}

fn programTiming(regs: anytype, queues: *const Queues, igs: u8, cycle_ns: u32, start: u64) void {
    regs.write32(off_igsc, igs);
    for (queues, 0..) |q, i| regs.write32(off_enc + 4 * i, q.count & mask_aen);
    regs.write32(off_cstc0, @truncate(start));
    regs.write32(off_cstc1, @truncate(start >> 32));
    regs.write32(off_ctc, cycle_ns);
}

/// Figure 32.8: start the TAS RAM init, wait for TASRR.
pub fn ramReset(regs: anytype, ops: anytype) u16 {
    regs.write32(off_rirm, rirm_start);
    const e = wait(regs, off_rirm, rirm_ready, true, spins);
    if (e != ok) ops.logError("etha_tas_ram_reset: EATASRIRM.TASRR never asserted");
    return e;
}

/// Figure 32.11. Commit sets TASE always and TASCC only when this replaces
/// a running schedule.
pub fn setSchedule(regs: anytype, ops: anytype, queues: *const Queues, igs: u8, cycle_ns: u32, start: u64) u16 {
    const v = validate(ops, queues);
    if (v != ok) return v;
    const tasc = regs.read32(off_tasc);
    if (tasc & tasc_tasci != 0) {
        ops.logError("etha_set_tas_schedule: EATASC.TASCI set");
        return busy;
    }
    programTiming(regs, queues, igs, cycle_ns, start);
    const l = learnAll(regs, ops, queues, (tasc >> 16) & mask_gal);
    if (l != ok) return l;
    var commit = tasc | tasc_tase;
    if (tasc & tasc_tase != 0) commit |= tasc_tascc else commit &= ~tasc_tascc;
    regs.write32(off_tasc, commit);
    return ok;
}

/// Figure 32.15: address, poll GR clear, then read the result.
pub fn readEntry(regs: anytype, ops: anytype, address: u8, out: *Entry) u16 {
    regs.write32(off_gr, address);
    const e = wait(regs, off_grr, busy_bit, false, spins);
    if (e != ok) {
        ops.logError("etha_read_tas_entry: EATASGRR.GR never cleared");
        return e;
    }
    const grr = regs.read32(off_grr);
    out.* = .{ .gate_time_ns = grr & mask_gtl, .gate_open = grr & gate_state_bit != 0 };
    return ok;
}

pub fn enable(regs: anytype, on: bool) u16 {
    const tasc = regs.read32(off_tasc);
    regs.write32(off_tasc, if (on) tasc | tasc_tase else tasc & ~tasc_tase);
    return ok;
}
