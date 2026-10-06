//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The DOTF REG00/REG03 sink shared by dotf_ctrl_abi.zig and
//! dotf_key_abi.zig (RA8FW-840). No exports.

const status = @import("internal/dotf_status.zig");

const off_reg03: usize = 0x8C;

pub const Regs = struct {
    base: usize,

    /// Channel already range-checked.
    pub fn of(channel: u8) Regs {
        return .{ .base = status.base + @as(usize, channel) * status.stride };
    }
    fn word(self: Regs, off: usize) *volatile u32 {
        return @ptrFromInt(self.base + off);
    }
    pub fn writeReg00(self: Regs, v: u32) void {
        self.word(status.off_reg00).* = v;
    }
    pub fn writeReg03(self: Regs, v: u32) void {
        self.word(off_reg03).* = v;
    }
};
