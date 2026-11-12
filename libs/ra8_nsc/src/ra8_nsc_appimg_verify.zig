//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Non-Secure Callable veneer for the Ed25519 verify backend
//! (RA8FW-291). The Non-Secure half of a TrustZone image reaches the
//! production `ra8_appimg_verify_fn` only through this file: a
//! secure-gateway entry in `.gnu.sgstubs` whose body validates every
//! pointer argument against the Non-Secure region before anything
//! secure is touched, then forwards to `ra8_appimg_ed25519_verify`.
//!
//! It is the Zig mirror of the C veneer contract in
//! `inc/ra8_nsc_veneer.h`: `RA8_NSC_VENEER` becomes the naked SG
//! entry, and `RA8_NSC_CHECK_NS_RANGE_R` becomes the TT-based range
//! probe below, byte-for-byte the expansion clang gives
//! `cmse_check_address_range(p, len, CMSE_NONSECURE | CMSE_MPU_READ)`.
//! The CMSE attribute stays in C headers only; nothing here names it.
//!
//! The range probe's decision logic is host-testable; the TT probe and
//! the SG entry are thumb-only and compile only on ARM targets.

const std = @import("std");
const builtin = @import("builtin");

const is_thumb = builtin.cpu.arch == .thumb or builtin.cpu.arch == .thumbeb;

/// The `ra8_appimg_msg_t` wire layout, mirrored from
/// `libs/ra8_app/src/internal/ed25519_backend.zig`. The veneer reads the
/// struct in Non-Secure memory, so its layout must match the backend's
/// exactly; the comptime asserts pin it.
pub const WireMsg = extern struct {
    head: ?[*]const u8 = null,
    tail: ?[*]const u8 = null,
    head_len: u32 = 0,
    tail_len: u32 = 0,
};

comptime {
    std.debug.assert(@offsetOf(WireMsg, "tail_len") + 4 == @sizeOf(WireMsg));
    std.debug.assert(@offsetOf(WireMsg, "tail") == @sizeOf(usize));
    std.debug.assert(@offsetOf(WireMsg, "head_len") == 2 * @sizeOf(usize));
    std.debug.assert(@offsetOf(WireMsg, "tail_len") == 2 * @sizeOf(usize) + 4);
}

/// The backend the veneer forwards to, as the C ABI links it out of
/// libra8_app.a. Same contract as `ra8_appimg_verify_fn`: only
/// k_ra8_ok admits.
pub extern fn ra8_appimg_ed25519_verify(
    ctx: ?*anyopaque,
    msg: ?*const WireMsg,
    signature: ?[*]const u8,
    public_key: ?[*]const u8,
) callconv(.c) u16;

/// CMSE_MPU_READ: the TT result bit position clang's
/// `cmse_check_address_range` expansion tests for readable.
pub const tt_read_bit: u32 = 1 << 20;

/// The pure decision of the TT-based NS range check, split from the
/// privileged TT probe so the host tests can drive it. It is the exact
/// expansion of `cmse_check_address_range(p, len, CMSE_NONSECURE |
/// CMSE_MPU_READ)`:
///
/// - a zero length or a range that wraps the 32-bit space fails;
/// - start and end addresses more than one 32-byte TT granule apart
///   must carry the same TT information (one region covers the range);
/// - the range is readable in the tested (Non-Secure) state;
/// - NULL is never a valid range, whatever region zero grants.
pub fn range_accepts(p: u32, len: u32, start_info: u32, last_info: u32) bool {
    if (len == 0) return false;
    const last = p +% len -% 1;
    if (last < p) return false;
    if ((last ^ p) >= 32 and last_info != start_info) return false;
    return (start_info & tt_read_bit) != 0 and p != 0;
}

// The thumb-only veneer half (TT probe, secure body, SG entry) lives in
// its own file and is imported only on thumb targets, where those
// instructions exist; a host test build analyzes only the pure logic
// above and the tests.
comptime {
    if (is_thumb) {
        _ = @import("ra8_nsc_appimg_verify_veneer.zig");
    }
}

// Shared error codes the veneer half answers with before the backend
// ever runs. Same values the backend itself uses.
pub const k_err_invalid_arg: u16 = 0x103;
pub const k_err_null_ptr: u16 = 0x504;
