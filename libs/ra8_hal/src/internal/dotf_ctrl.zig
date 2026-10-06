//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF REG00 control (RA8FW-840, was part of ra8_dotf.c): assemble the
//! mode/key-size/SCA/enable word and arm, disarm or retune a channel. Pure
//! over a ChanState and a register sink with writeReg00; the exports are in
//! src/dotf_ctrl_abi.zig. HUM Ch 45.3 "Register Descriptions" p 3049.

const state = @import("dotf_state.zig");
/// Re-exported so host tests share the same ChanState type.
pub const state_mod = state;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;

/// REG00 fields (inc/ra8_dotf_regs.h).
pub const mode_ctr: u32 = 0x2000_0000;
pub const aes_enable: u32 = 0x0000_0200;
pub const sca_en: u32 = 0x0001_0000;
pub const sca_mode: u32 = 0x0002_0000;
pub const key_size_128: u32 = state.key_size_128;
pub const key_size_192: u32 = 0x0100_0000;
pub const key_size_256: u32 = 0x0300_0000;

/// ra8_dotf_sca_level_t.
pub const sca_off: u8 = 0;
pub const sca_standard: u8 = state.sca_standard;
pub const sca_max: u8 = 2;

pub fn validKeySize(size: u32) bool {
    return size == key_size_128 or size == key_size_192 or size == key_size_256;
}

pub fn validSca(level: u8) bool {
    return level <= sca_max;
}

/// internal_sca_bits: max sets enable and mode, standard sets enable only.
pub fn scaBits(level: u8) u32 {
    if (level == sca_max) return sca_en | sca_mode;
    if (level == sca_standard) return sca_en;
    return 0;
}

/// internal_assemble_reg00: CTR mode (HUM 45.1) | key size | SCA bits,
/// plus the AES enable when arming.
pub fn reg00(st: *const state.ChanState, armed: bool) u32 {
    var v = mode_ctr | st.cached_key_size | scaBits(st.cached_sca);
    if (armed) v |= aes_enable;
    return v;
}

pub fn enable(st: *state.ChanState, regs: anytype) void {
    regs.writeReg00(reg00(st, true));
    st.enabled = 1;
}

/// Writing 0 to REG00 puts the channel in bypass.
pub fn disable(st: *state.ChanState, regs: anytype) void {
    regs.writeReg00(state.reg00_disable);
    st.enabled = 0;
}

/// Cache the level; rewrite REG00 only while the channel is armed.
pub fn setSca(st: *state.ChanState, regs: anytype, level: u8) u16 {
    if (!validSca(level)) return invalid_arg;
    st.cached_sca = level;
    if (st.enabled != 0) regs.writeReg00(reg00(st, true));
    return ok;
}

/// Cache the size; rewrite REG00 only while the channel is armed.
pub fn setKeySize(st: *state.ChanState, regs: anytype, size: u32) u16 {
    if (!validKeySize(size)) return invalid_arg;
    st.cached_key_size = size;
    if (st.enabled != 0) regs.writeReg00(reg00(st, true));
    return ok;
}
