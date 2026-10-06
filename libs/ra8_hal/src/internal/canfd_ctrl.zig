//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! CANFD test mode, ISO framing select and module-stop gating (RA8FW-861,
//! was part of ra8_canfd.c). Exports live in src/canfd_ctrl_abi.zig.

/// CFDC[0].CTR (+0x4) CTME, bit 24. HUM Ch 41 "CFDCnCTR" p 2710.
pub const ctme: u32 = 1 << 24;
/// CFDC[0].CTR CTMS[1:0], bits [26:25].
pub const ctms_shift: u5 = 25;
pub const ctms_mask: u32 = 0x3 << ctms_shift;
/// Highest `ra8_ctms_mode_t` (`k_ra8_ctms_self_test_1`).
pub const ctms_max: u8 = 3;
/// CFDC[0].CTR offset from the channel base.
pub const off_ctr: usize = 0x004;
/// CFDGFDCFG offset; bit 0 is NISO (1 = ISO 11898-1). HUM Ch 41 "CFDGFDCFG".
pub const off_gfdcfg: usize = 0x0B0;
pub const niso: u32 = 1 << 0;

/// `ra8_chmdc_mode_t`.
pub const Mode = enum(c_uint) { operation = 0, reset = 1, halt = 2 };

/// MSTPC27 (CANFD0) and MSTPC26 (CANFD1), `k_ra8_mstp_canfd0/1`.
pub const mstp_ids = [_]u16{ (2 << 8) | 27, (2 << 8) | 26 };

pub const Error = error{InvalidMode};

/// CTR with any earlier CTME/CTMS cleared, then CTME and `mode` set.
pub fn testModeCtr(ctr: u32, mode: u8) u32 {
    const sel = (@as(u32, mode) << ctms_shift) & ctms_mask;
    return (ctr & ~(ctme | ctms_mask)) | ctme | sel;
}

/// CFDGFDCFG with NISO set for ISO framing, cleared for Bosch non-ISO.
pub fn isoValue(v: u32, enable: bool) u32 {
    return if (enable) v | niso else v & ~niso;
}

/// CTME/CTMS are writable only in CH_HALT: halt, write CTR, back to
/// operation. A failed halt still tries to return to operation and
/// reports the halt error. `hw` provides setMode, readCtr and writeCtr.
pub fn setTestMode(hw: anytype, mode: u8) Error!u16 {
    if (mode > ctms_max) return error.InvalidMode;
    const halt_err = hw.setMode(.halt);
    if (halt_err != 0) {
        _ = hw.setMode(.operation);
        return halt_err;
    }
    hw.writeCtr(testModeCtr(hw.readCtr(), mode));
    return hw.setMode(.operation);
}
