//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! RSIP-E50D device-security path (lifecycle / debug / tamper / DPA arm),
//! RA8FW-1064; replaces ra8_rsip_devsec.c. Fail-closed by contract: the
//! "RSIP security-state" registers below are not documented RA8D2 registers
//! (HUM Ch 51 "Security Features" is a prose feature index and HUM Ch 52
//! "Renesas Secure IP (RSIP-E50D)" has no register map; the real lifecycle /
//! debug-authorisation state lives in the DLM, option-setting memory and
//! SAU), so the guarded command path only round-trips the host register fake.
//! The C keyed that split on `RA8_INSECURE_STUB_CRYPTO ||
//! RA8_OFF_TARGET`; the Zig ABI keys it on the build's hosted-ness. A
//! freestanding (production) image gets every entry point returning
//! not_supported and writing no fabricated state: a security-state query that
//! fabricates a plausible-looking "secure" answer is worse than one that
//! refuses.
//!
//! The logic is generic over `Hw` (read32/write32 on an RSIP offset) so host
//! tests drive a fake register file with no address mapping. `guarded` is
//! false exactly when the caller must be refused.

pub const base: usize = 0x403B_0000;

/// The invented "security-state" window offsets, kept for the guarded host
/// path only. They carry no HUM citation: there is no real register map.
pub const off_ctrl: usize = 0x0000;
pub const off_tamper_ctrl: usize = 0x0014;
pub const off_tamper_status: usize = 0x0018;
pub const off_dpa_ctrl: usize = 0x001C;
pub const off_life_state: usize = 0x00C0;
pub const off_debug_level: usize = 0x00C4;

/// CTRL.DPA_ARM bit.
pub const mask_ctrl_dpa_arm: u32 = 0x0000_0008;
/// Union of all tamper sources (k_ra8_rsip_tamper_src_all).
pub const tamper_src_all: u32 = 0x0007_003F;
/// Highest lifecycle state (k_ra8_rsip_life_rma).
pub const life_rma: u32 = 5;
/// Highest debug level (k_ra8_rsip_debug_al2).
pub const debug_al2: u8 = 2;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const not_supported: u16 = 0x107;

pub fn lifeGet(hw: anytype, guarded: bool, out: *u32) u16 {
    if (!guarded) return not_supported;
    out.* = hw.read32(off_life_state);
    return ok;
}

pub fn lifeAdvance(hw: anytype, guarded: bool, state: u32) u16 {
    if (!guarded) return not_supported;
    if (state > life_rma) return invalid_arg;
    const cur = hw.read32(off_life_state);
    if (state < cur) return invalid_state;
    hw.write32(off_life_state, state);
    return ok;
}

pub fn debugLevelGet(hw: anytype, guarded: bool, out: *u8) u16 {
    if (!guarded) return not_supported;
    out.* = @truncate(hw.read32(off_debug_level));
    return ok;
}

pub fn debugLevelSet(hw: anytype, guarded: bool, level: u8) u16 {
    if (!guarded) return not_supported;
    if (level > debug_al2) return invalid_arg;
    hw.write32(off_debug_level, level);
    return ok;
}

pub fn tamperEnable(hw: anytype, guarded: bool, sources: u32) u16 {
    if (!guarded) return not_supported;
    if ((sources & ~tamper_src_all) != 0) return invalid_arg;
    hw.write32(off_tamper_ctrl, sources);
    return ok;
}

pub fn tamperStatus(hw: anytype, guarded: bool, out: *u32) u16 {
    if (!guarded) return not_supported;
    out.* = hw.read32(off_tamper_status);
    return ok;
}

pub fn tamperAck(hw: anytype, guarded: bool, mask: u32) u16 {
    if (!guarded) return not_supported;
    if (mask == 0) return invalid_arg;
    if ((mask & ~tamper_src_all) != 0) return invalid_arg;
    hw.write32(off_tamper_status, mask);
    return ok;
}

pub fn dpaArm(hw: anytype, guarded: bool, enable: bool) u16 {
    if (!guarded) return not_supported;
    const ctrl = hw.read32(off_ctrl);
    hw.write32(off_ctrl, if (enable) ctrl | mask_ctrl_dpa_arm else ctrl & ~mask_ctrl_dpa_arm);
    hw.write32(off_dpa_ctrl, @intFromBool(enable));
    return ok;
}
