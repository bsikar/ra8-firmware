//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Register write protection (PRCR, HUM Ch 13). The Zig twin of the C
//! `RA8_PROTECTED_WRITE` scope (inc/ra8_register_protection.h):
//!
//!     const window = prcr.open(reg, prcr.unlock_sar);
//!     defer window.close();
//!
//! check_attribution_gates.py treats everything from `prcr.open(` to the
//! end of the enclosing block as gated (RA8FW-546).

/// PRCR_S, R_SYSTEM 0x4001E000 + 0x3FA (inc/ra8_system_regs.h).
pub const addr: usize = 0x4001E3FA;
/// Mandatory password in the upper byte (`k_ra8_prcr_key`).
pub const key: u16 = 0xA500;
/// PRC4: security attribution registers (`k_ra8_prcr_grp4_sar`).
pub const grp4_sar: u16 = 0x0010;
/// `k_ra8_prcr_unlock_sar`.
pub const unlock_sar: u16 = key | grp4_sar;
/// `k_ra8_prcr_lock_all`: password with every group bit clear.
pub const lock_all: u16 = key;

/// An open protection window; `close` re-locks every group.
pub const Window = struct {
    reg: *volatile u16,

    /// Re-lock every protection group (`ra8_sys_prcr_lock_all`).
    pub fn close(window: Window) void {
        window.reg.* = lock_all;
    }
};

/// Unlock the groups in `unlock` (password included) and return the window.
pub fn open(reg: *volatile u16, unlock: u16) Window {
    reg.* = unlock;
    return .{ .reg = reg };
}
