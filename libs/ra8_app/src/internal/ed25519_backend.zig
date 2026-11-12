//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The production Ed25519 verify backend (RA8FW-291): the platform's real
//! supply of the `ra8_appimg_verify_fn` seam. The admission gate
//! (`appimg_verify.zig`) stays primitive-free on purpose, calling whatever
//! the verifier's `verify` member points at, and this file is what
//! production points that member at.
//!
//! Verify is the software Ed25519 in Zig's standard library, the same
//! primitive the packer signs with, so an image the signer produces is an
//! image the loader accepts. The pinned public key arrives with the verifier
//! from secure storage; the backend never reads anything but the signed runs
//! the gate passes it.

const std = @import("std");

const Ed25519 = std.crypto.sign.Ed25519;

/// The signed material as the C declares it (`ra8_appimg_msg_t`): the two
/// pointers, then the two lengths. The layout mirrors the unchanged C
/// header, held in pointer-width multiples the way the ABI membrane asserts
/// its caller-owned structs.
pub const WireMsg = extern struct {
    head: ?[*]const u8 = null,
    tail: ?[*]const u8 = null,
    head_len: u32 = 0,
    tail_len: u32 = 0,
};

comptime {
    const word = @sizeOf(usize);
    std.debug.assert(@offsetOf(WireMsg, "tail") == word);
    std.debug.assert(@offsetOf(WireMsg, "head_len") == 2 * word);
    std.debug.assert(@offsetOf(WireMsg, "tail_len") == 2 * word + 4);
}

/// The `ra8_err_t` subset this backend answers with. Only `ok` is an
/// admission; the gate maps every other code to a refusal and keeps only
/// `not_supported` distinct, and a backend with a working primitive never
/// has that one.
pub const Code = enum(u16) {
    ok = 0,
    invalid_arg = 0x103,
    not_supported = 0x107,
    validation_failed = 0x501,
    null_ptr = 0x504,
};

fn code(err: Code) u16 {
    return @backingInt(err);
}

/// The backend as the C links it: a `ra8_appimg_verify_fn` whose ctx,
/// two-run message, signature and pinned public key arrive as raw C
/// pointers.
///
/// Only `k_ra8_ok` admits. A signature that fails to verify is
/// `k_ra8_err_validation_failed`, and a key or signature whose encoding the
/// primitive rejects is `k_ra8_err_invalid_arg`; both are refusals to the
/// gate. A NULL argument is refused the same way.
pub export fn ra8_appimg_ed25519_verify(
    ctx: ?*anyopaque,
    msg: ?*const WireMsg,
    signature: ?[*]const u8,
    public_key: ?[*]const u8,
) callconv(.c) u16 {
    _ = ctx;
    const wire = msg orelse return code(.null_ptr);
    const signature_bytes = signature orelse return code(.null_ptr);
    const key_bytes = public_key orelse return code(.null_ptr);
    const head = wire.head orelse return code(.null_ptr);
    const tail = wire.tail orelse return code(.null_ptr);

    const key = Ed25519.PublicKey.fromBytes(key_bytes[0..Ed25519.PublicKey.encoded_length].*) catch
        return code(.invalid_arg);
    var verifier = Ed25519.Signature.fromBytes(
        signature_bytes[0..Ed25519.Signature.encoded_length].*,
    ).verifier(key) catch return code(.invalid_arg);
    verifier.update(head[0..wire.head_len]);
    verifier.update(tail[0..wire.tail_len]);
    verifier.verify() catch return code(.validation_failed);
    return code(.ok);
}
