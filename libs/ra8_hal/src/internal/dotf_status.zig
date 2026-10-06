//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF REG00 self-test and raw status read (RA8FW-827, part of ra8_dotf.c).
//! Pure over a `regs` value; the exports live in src/dotf_status_abi.zig.

pub const base: usize = 0x4026_8800;
pub const stride: usize = 0x100;
pub const off_reg00: usize = 0x80;
pub const reg00_self_test: u32 = 0x0010_0000;
pub const self_test_spin: u32 = 8;

/// Address of REG00 for a channel already range-checked.
pub fn reg00(channel: u8) usize {
    return base + @as(usize, channel) * stride + off_reg00;
}

pub const SelfTest = struct { done: bool, status: u32 };

/// Trigger BIST, poll up to `self_test_spin` times for the bit to clear
/// (`done(reg, iter, cond)` lets host builds arm the wait), read the status,
/// then restore the saved REG00.
pub fn selfTest(regs: anytype, done: anytype) SelfTest {
    const saved = regs.read();
    regs.write(saved | reg00_self_test);
    var ok = false;
    var i: u32 = 0;
    while (i < self_test_spin) : (i += 1) {
        if (done.eval(i, regs.read() & reg00_self_test == 0)) {
            ok = true;
            break;
        }
    }
    const status = regs.read();
    regs.write(saved);
    return .{ .done = ok, .status = status };
}
