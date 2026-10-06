//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! ETHA credit-based shaper and error counters (RA8FW-816, was part of
//! ra8_etha.c). Pure: one port's ETHA block comes in as a `regs` value
//! (read32/write32 by offset). Port, class and pointer checks stay in
//! src/etha_cbs_abi.zig.

pub const tc_count = 8;

pub const off_eacaec: usize = 0x200;
pub const off_eacc: usize = 0x204;
pub const off_eacaivc: usize = 0x220; // EACAIVCq, 8 x u32
pub const off_eacaulc: usize = 0x240; // EACAULCq, 8 x u32
pub const off_eacoem: usize = 0x260;
pub const off_eacoivm: usize = 0x280; // EACOIVMq, 8 x u32
pub const off_eacoulm: usize = 0x2A0; // EACOULMq, 8 x u32
pub const off_eacgsm: usize = 0x2C0;
pub const off_counters: usize = 0x400; // EAUSMFSECN, EATFECN, EAFSECN, EADQOECN, EADQSECN

pub const mask_civ: u32 = 0xF_FFFF;
pub const mask_cul: u32 = 0x7FFF_FFFF;
pub const mask_counter: u32 = 0xFFFF;
pub const counter_count = 5;

/// `ra8_etha_cbs_param_t`.
pub const Param = extern struct { increment: u32, upper_lim: u32 };
/// `ra8_etha_stats_t`, counters in register order.
pub const Counters = extern struct { values: [counter_count]u16 };
/// What `ra8_etha_get_cbs_state` reports for one class.
pub const State = struct { enabled: u8, gate_open: u8, oper: Param };

pub fn tcOk(tc: u8) bool {
    return tc < tc_count;
}

pub fn paramOk(p: *const Param) bool {
    return p.increment <= mask_civ and p.upper_lim <= mask_cul;
}

fn at(base: usize, tc: u8) usize {
    return base + 4 * @as(usize, tc);
}

/// Enabling writes the admin increment and limit first, then sets the class
/// bit in EACAEC and EACC; disabling clears the bit (HUM 32.3.4.1-4 p 1642-1643).
pub fn configure(regs: anytype, tc: u8, param: ?*const Param) void {
    const bit = @as(u32, 1) << @intCast(tc);
    if (param) |p| {
        regs.write32(at(off_eacaivc, tc), p.increment & mask_civ);
        regs.write32(at(off_eacaulc, tc), p.upper_lim & mask_cul);
        regs.write32(off_eacaec, regs.read32(off_eacaec) | bit);
        regs.write32(off_eacc, regs.read32(off_eacc) | bit);
    } else {
        regs.write32(off_eacaec, regs.read32(off_eacaec) & ~bit);
        regs.write32(off_eacc, regs.read32(off_eacc) & ~bit);
    }
}

/// Oper enable, gate state and oper increment/limit (HUM 32.3.4.5-8 p 1644-1645).
pub fn state(regs: anytype, tc: u8) State {
    const bit = @as(u32, 1) << @intCast(tc);
    return .{
        .enabled = @intFromBool(regs.read32(off_eacoem) & bit != 0),
        .gate_open = @intFromBool(regs.read32(off_eacgsm) & bit != 0),
        .oper = .{
            .increment = regs.read32(at(off_eacoivm, tc)) & mask_civ,
            .upper_lim = regs.read32(at(off_eacoulm, tc)) & mask_cul,
        },
    };
}

/// The five 16-bit error counters (HUM 32.3.6.1-5 p 1656-1659).
pub fn readCounters(regs: anytype) Counters {
    var c: Counters = undefined;
    for (&c.values, 0..) |*v, i| v.* = @intCast(regs.read32(off_counters + 4 * i) & mask_counter);
    return c;
}

pub fn clearCounters(regs: anytype) void {
    for (0..counter_count) |i| regs.write32(off_counters + 4 * i, 0);
}
