//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The C membrane for `ra8_secure_app`: every symbol `inc/key_vault.h`,
//! `inc/ota_commit.h`, and `src/secure_trng_internal.h` declare, and nothing
//! else. Those headers are unchanged, so the NSC veneers, the host test
//! suites, and `secure_app_vault_demo` link against this archive without
//! knowing the bodies moved to Zig.
//!
//! NUL-terminated strings and raw pointers stop here. Everything past this file
//! works in slices and typed enums.

const std = @import("std");

const ota = @import("internal/ota.zig");
const trng = @import("internal/trng.zig");
const vault = @import("internal/vault.zig");
const vocab = @import("internal/vocab.zig");

const Err = vocab.Err;

comptime {
    // The headers spell `ra8_err_t` as `enum : uint16_t`, so the return width
    // has to match on every target the archive is built for.
    std.debug.assert(@sizeOf(Err) == 2);
}

// ---------------------------------------------------------------------------
// key_vault.h
// ---------------------------------------------------------------------------

export fn ra8_key_vault_init() u16 {
    return vault.init().code();
}

export fn ra8_key_vault_store(slot: u16, key: ?[*]const u8) u16 {
    const src = key orelse return Err.null_ptr.code();
    return vault.store(slot, src[0..vault.Limits.key_bytes]).code();
}

export fn ra8_key_vault_sha256_xor_challenge(
    slot: u16,
    challenge: ?[*]const u8,
    out: ?[*]u8,
) u16 {
    const chal = challenge orelse return Err.null_ptr.code();
    const dst = out orelse return Err.null_ptr.code();
    return vault.sha256XorChallenge(
        slot,
        chal[0..vault.Limits.chal_bytes],
        dst[0..vault.Limits.digest_bytes],
    ).code();
}

export fn ra8_key_vault_set_mac_key(key: ?[*]const u8, key_len: u16) u16 {
    const src = key orelse return Err.null_ptr.code();
    // The length is validated inside; an out-of-range one must not be used to
    // build the slice first.
    if (key_len != vault.Limits.mac_key_128 and key_len != vault.Limits.mac_key_256) {
        return Err.invalid_arg.code();
    }
    return vault.setMacKey(src[0..key_len]).code();
}

export fn ra8_key_vault_load_mac_key(out: ?[*]u8, out_cap: u16, out_len: ?*u16) u16 {
    const dst = out orelse return Err.null_ptr.code();
    const len = out_len orelse return Err.null_ptr.code();
    return vault.loadMacKey(dst[0..out_cap], len).code();
}

// ---------------------------------------------------------------------------
// secure_trng_internal.h
// ---------------------------------------------------------------------------

export fn priv_ra8_secure_trng_reset() u16 {
    return trng.reset().code();
}

export fn priv_ra8_secure_trng_read(out: ?[*]u8, len: u32) u16 {
    const dst = out orelse return Err.null_ptr.code();
    // The cap is validated inside; do not build an oversized slice to get there.
    if (len == 0 or len > trng.Limits.max_bytes) {
        return if (trng.enabled) Err.invalid_arg.code() else Err.not_supported.code();
    }
    return trng.read(dst[0..len]).code();
}

// ---------------------------------------------------------------------------
// ota_commit.h
// ---------------------------------------------------------------------------

export fn ra8_ota_commit_reset() u16 {
    return ota.reset().code();
}

export fn ra8_ota_commit_swap_bank(target: u8) u16 {
    return ota.swapBank(target).code();
}

export fn ra8_ota_commit_pending(out_target: ?*u8) u16 {
    const dst = out_target orelse return Err.null_ptr.code();
    var bank: ota.Bank = undefined;
    const err = ota.pendingTarget(&bank);
    if (err != .ok) return err.code();
    dst.* = @intFromEnum(bank);
    return Err.ok.code();
}

export fn ra8_ota_commit_set_bank_config(raw_value: u32) u16 {
    return ota.setBankConfig(raw_value).code();
}

export fn ra8_ota_commit_get_bank_config(out_value: ?*u32) u16 {
    const dst = out_value orelse return Err.null_ptr.code();
    dst.* = ota.bankConfig();
    return Err.ok.code();
}
