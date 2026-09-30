//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Secure-side OTA bank commit and bank-config shadow.
//!
//! Port of the former `src/ota_commit.c`. Both writes here are option-region
//! programs (an OFS3 / BTFLG option byte behind the PRCR unlock): brick-risky,
//! bench-gated, and not wired. So on silicon they are FAIL-CLOSED (T5-10): the
//! argument and idempotency checks run, then the call returns `not_supported`
//! rather than a fake `ok` for a commit that never touched flash. Only an
//! off-target build keeps an in-memory shadow, so the masking and single-shot
//! policy stay unit-testable.
//!
//! Note the guard here is `RA8_OFF_TARGET` alone, not the vault's wider
//! insecure-stub condition: an insecure dev image still must not arm a real
//! boot-bank swap.

const build_config = @import("build_config");

const vocab = @import("vocab.zig");

pub const Err = vocab.Err;

/// Whether this image keeps the host shadow instead of failing closed.
pub const shadowed = build_config.off_target;

/// Bank selector.
pub const Bank = enum(u8) {
    a = 0,
    b = 1,

    /// The C took a raw `uint8_t` across the ABI and validated it here.
    pub fn fromRaw(raw: u8) ?Bank {
        return switch (raw) {
            0 => .a,
            1 => .b,
            else => null,
        };
    }
};

/// Allowed-bit masks for the bank-config register.
pub const Mask = struct {
    /// Only the 2-bit BANK_SEL field; everything else (debug-disable,
    /// integrity-check mode, lifetime fuses) is filtered out before the write.
    pub const allowed: u32 = 0x0000_0003;
};

var pending: bool = false;
var pending_target: Bank = .a;
var bank_config: u32 = 0;

/// Drop any pending commit and zero the bank-config shadow.
pub fn reset() Err {
    pending = false;
    pending_target = .a;
    bank_config = 0;
    return .ok;
}

/// Arm the boot ROM to start from `target` on next reset.
///
/// Fail-closed on silicon: nothing is armed and nothing is written.
pub fn swapBank(raw_target: u8) Err {
    const target = Bank.fromRaw(raw_target) orelse return .invalid_arg;
    if (pending) return .invalid_state;
    if (!shadowed) {
        // TODO(real OFS3/BTFLG boot-bank option-byte swap write -- bench-gated,
        // brick-risky): unlock PRCR, program the boot option byte via ra8_flash_*,
        // re-lock PRCR, confirm by read-back, then arm the shadow and return ok.
        return .not_supported;
    }
    pending_target = target;
    pending = true;
    return .ok;
}

/// Read back the pending swap target.
pub fn pendingTarget(out_target: *Bank) Err {
    if (!pending) return .no_data;
    out_target.* = pending_target;
    return .ok;
}

/// Write the bank-config register, masked to the allowed bits.
///
/// Fail-closed on silicon: the masked value is computed but never persisted.
pub fn setBankConfig(raw_value: u32) Err {
    const masked = raw_value & Mask.allowed;
    if (!shadowed) {
        // TODO(real bank-config option-region write -- bench-gated, brick-risky):
        // unlock PRCR, program `masked` into the option region, re-lock, confirm.
        return .not_supported;
    }
    bank_config = masked;
    return .ok;
}

/// Read back the bank-config shadow.
pub fn bankConfig() u32 {
    return bank_config;
}
