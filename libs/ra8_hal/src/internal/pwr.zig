//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Power-management glue: wake-source decode and the WUPEN0/WUPEN1
//! enables in the SYSTEM block (HUM Ch 14.2.19-20). Port of ra8_pwr.c
//! (RA8FW-570); the C ABI lives in pwr_abi.zig.

const builtin = @import("builtin");

pub const system_base: usize = 0x4001E000;
pub const off_wupen0: usize = 0x1A0;
pub const off_wupen1: usize = 0x1A4;
pub const wupen_count: u8 = 2;
pub const wupen_bits: u8 = 32;

/// True in a freestanding image, false in a host test binary.
pub const on_target = builtin.os.tag == .freestanding;

/// A decoded `ra8_pwr_wake_t`: (register << 8) | bit.
pub const Wake = struct {
    reg: u1,
    bit: u5,

    pub fn decode(source: u16) ?Wake {
        const reg = source >> 8;
        const bit = source & 0xFF;
        if (reg >= wupen_count or bit >= wupen_bits) return null;
        return .{ .reg = @intCast(reg), .bit = @intCast(bit) };
    }

    pub fn mask(w: Wake) u32 {
        return @as(u32, 1) << w.bit;
    }
};

pub const Block = struct {
    base: usize = system_base,

    fn wupen(b: Block, reg: u1) *volatile u32 {
        return @ptrFromInt(b.base + if (reg == 0) off_wupen0 else off_wupen1);
    }

    pub fn enable(b: Block, w: Wake) void {
        const p = b.wupen(w.reg);
        p.* = p.* | w.mask();
    }

    pub fn disable(b: Block, w: Wake) void {
        const p = b.wupen(w.reg);
        p.* = p.* & ~w.mask();
    }

    pub fn isEnabled(b: Block, w: Wake) bool {
        return (b.wupen(w.reg).* & w.mask()) != 0;
    }

    /// Software Standby needs at least one armed wake source.
    pub fn anyArmed(b: Block) bool {
        const w0 = b.wupen(0).*;
        const w1 = b.wupen(1).*;
        return w0 != 0 or w1 != 0;
    }
};

/// Idle until the next interrupt; a no-op in a host test binary.
pub fn waitForInterrupt() void {
    if (!on_target) return;
    asm volatile ("wfi" ::: "memory");
}
