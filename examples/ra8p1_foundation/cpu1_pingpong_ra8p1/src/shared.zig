//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The four words both cores of cpu1_pingpong_ra8p1 agree on, at the base of
//! the RA8P1 shared window (ra8_board_ra8p1 system_init region 4,
//! 0x2210_0000), the same layout and magics the EK-RA8D2 cpu1_pingpong uses.

pub const address: usize = 0x2210_0000;
pub const magic_ping: u32 = 0x1234;
pub const magic_pong: u32 = 0x4321;
/// Round trips the M85 completes before it prints PASS.
pub const rounds: u32 = 10;

pub const Block = extern struct {
    /// The M85 bumps it after writing `ping_payload`.
    ping_seq: u32,
    /// CPU1 copies `ping_seq` here after writing `pong_payload`.
    pong_seq: u32,
    ping_payload: u32,
    pong_payload: u32,
};

pub fn block() *volatile Block {
    return @ptrFromInt(address);
}
