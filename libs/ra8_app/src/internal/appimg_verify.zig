//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `.ra8app` admission decision: parse, grant, signature, in that order.
//! Default-deny throughout. The Ed25519 primitive is not here; it arrives as
//! a `Backend` seam, which is what lets the policy be proven off target while
//! the primitive is proven where it lives.
//!
//! The backend is trusted for exactly one thing: success means the signature
//! verified. Every other answer, recognised or not, is a refusal, so there is
//! no path on which an unrecognised backend verdict becomes an admission.

const std = @import("std");
const appimg = @import("appimg.zig");

/// The container format this gate admits. Re-exported so a consumer
/// reaches the header and span types through the gate it is already
/// importing, rather than binding `appimg.zig` a second time.
pub const image = appimg;

/// Ed25519 public-key length, bytes.
pub const pubkey_bytes: usize = 32;

/// Why an image was refused admission.
pub const Error = appimg.Error || error{
    /// The app declares a capability the host withheld.
    AccessDenied,
    /// The image carries no signature, or the signature did not verify.
    BadSignature,
};

/// The signed material, in signing order.
///
/// Two runs rather than one buffer: the signature covers the header up to the
/// signature field and then the payload, and those are not contiguous. Slices
/// carry the lengths, so a caller cannot pair one run's pointer with another's
/// length.
pub const Message = struct {
    /// Header prefix, up to where the signature field begins.
    head: []const u8,
    /// The module payload that follows the header.
    tail: []const u8,
};

/// Verdict a verify backend may return.
///
/// Three outcomes, not an open integer: `unsupported` is the one refusal the
/// gate reports verbatim, because a platform with no Ed25519 at all is a
/// different fact from an image that failed to verify.
pub const Verdict = enum {
    /// The signature is cryptographically valid under the pinned key.
    good,
    /// The signature did not verify.
    bad,
    /// This platform implements no Ed25519 verify.
    unsupported,
};

/// The Ed25519 verify seam the platform supplies.
pub const Backend = struct {
    /// Verifies `signature` over `msg.head || msg.tail` under `public_key`.
    verify: *const fn (
        ctx: ?*anyopaque,
        msg: Message,
        signature: *const [appimg.Width.signature]u8,
        public_key: *const [pubkey_bytes]u8,
    ) Verdict,
    /// Opaque backend context, handed through unchanged.
    ctx: ?*anyopaque = null,
    /// The pinned root public key.
    public_key: *const [pubkey_bytes]u8,
    /// Capability bits the host is willing to give.
    granted: u32,
};

/// Whether a signature field is the all-zero "unsigned" pattern.
///
/// An unsigned `.ra8app` is spelled by leaving the field zero, which is what a
/// builder that never ran the signer produces. Refusing it here rather than in
/// the backend keeps "unsigned modules are rejected" true even against a
/// backend that would happily verify against a zero signature.
fn signatureAbsent(signature: []const u8) bool {
    return std.mem.allEqual(u8, signature, 0);
}

/// Describe the signed material of an already-parsed image.
///
/// Exposed on its own because the signer side needs it without needing a
/// verify backend.
pub fn signedMessage(header: appimg.Header, bytes: []const u8) appimg.Error!Message {
    const head = try appimg.signedSpan(bytes.len);
    const tail = try appimg.payloadSpan(header, bytes.len);
    return .{
        .head = bytes[head.offset..][0..head.length],
        .tail = bytes[tail.offset..][0..tail.length],
    };
}

/// Decide whether an image may be loaded, and return the header if it may.
///
/// Ordered, and the order is the policy: the container is proven before any
/// field of it is read, the manifest is compared before a signature is spent,
/// and an unsigned image is refused without consulting the backend at all.
pub fn verify(backend: Backend, bytes: []const u8) Error!appimg.Header {
    const header = try appimg.parse(bytes);

    if (appimg.capabilitiesPermitted(header, backend.granted)) |refusal| {
        return switch (refusal) {
            .malformed_grant => appimg.Error.Validation,
            .withheld => Error.AccessDenied,
        };
    }

    if (signatureAbsent(&header.signature)) return appimg.Error.Validation;

    const msg = try signedMessage(header, bytes);
    return switch (backend.verify(backend.ctx, msg, &header.signature, backend.public_key)) {
        .good => header,
        .unsupported => appimg.Error.Unsupported,
        .bad => Error.BadSignature,
    };
}
