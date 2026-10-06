//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `.ra8app` packer: lay out the header and the module payload, then sign
//! `head || tail` with Ed25519, the exact message `appimg_verify.signedMessage`
//! names. Host side only: it allocates the file image, and the signing key
//! never reaches the target.
//!
//! Every image is proven by `appimg.parse` before it is signed, so the packer
//! cannot emit a container the in-tree parser would refuse.

const std = @import("std");
const appimg = @import("appimg.zig");

/// The admission gate the packed image must pass. Re-exported so a host tool
/// reaches the verifier through the packer it already imports.
pub const gate = @import("appimg_verify.zig");

const Ed25519 = std.crypto.sign.Ed25519;
const Header = appimg.Header;

/// What a module declares about itself, in host types.
pub const Manifest = struct {
    /// Entry point, as a byte offset into the code segment.
    entry_offset: u32,
    /// Stack the loader must give the module, bytes.
    stack_size: u32,
    min_api_version: u32 = appimg.Format.api_version_current,
    capabilities: u32 = appimg.Capability.none,
    /// Stable identifier, shorter than `appimg.Width.app_id`.
    app_id: []const u8,
    /// Name a launcher shows, shorter than `appimg.Width.display_name`.
    display_name: []const u8,
};

/// Why the packer refused to build or sign an image.
pub const Error = appimg.Error || std.mem.Allocator.Error || error{
    /// A text field does not fit its fixed width with its NUL.
    NameTooLong,
    /// The key pair cannot produce a signature.
    KeyRejected,
};

/// Copy `text` into a NUL-padded fixed-width field.
fn field(comptime width: usize, text: []const u8) Error![width]u8 {
    if (text.len >= width) return Error.NameTooLong;
    var out: [width]u8 = @splat(0);
    @memcpy(out[0..text.len], text);
    return out;
}

/// The unsigned header for a payload of `code_len` + `data_len` bytes.
pub fn header(manifest: Manifest, code_len: usize, data_len: usize) Error!Header {
    const limit = std.math.maxInt(u32);
    if (code_len > limit or data_len > limit) return appimg.Error.OutOfRange;
    return .{
        .magic = appimg.Format.magic,
        .format_version = appimg.Format.version,
        .entry_offset = manifest.entry_offset,
        .code_size = @intCast(code_len),
        .data_size = @intCast(data_len),
        .stack_size = manifest.stack_size,
        .min_api_version = manifest.min_api_version,
        .capabilities = manifest.capabilities,
        .app_id = try field(appimg.Width.app_id, manifest.app_id),
        .display_name = try field(appimg.Width.display_name, manifest.display_name),
        .signature = @splat(0),
    };
}

/// Lay out `header || code || data` with an all-zero (unsigned) signature.
/// The caller owns the returned bytes.
pub fn assemble(
    allocator: std.mem.Allocator,
    manifest: Manifest,
    code: []const u8,
    data: []const u8,
) Error![]u8 {
    const head = try header(manifest, code.len, data.len);
    const bytes = try allocator.alloc(u8, head.imageLen());
    errdefer allocator.free(bytes);

    const code_at = @sizeOf(Header);
    @memcpy(bytes[0..code_at], std.mem.asBytes(&head));
    @memcpy(bytes[code_at..][0..code.len], code);
    @memcpy(bytes[code_at + code.len ..][0..data.len], data);
    _ = try appimg.parse(bytes);
    return bytes;
}

/// Sign an assembled image in place over the runs the verifier reads.
///
/// RFC 8032 deterministic Ed25519: one input and one key give one file, so a
/// rebuilt `.ra8app` is byte-identical. The incremental signer is hedged with
/// fresh randomness, so the two runs are joined into one scratch message.
pub fn sign(allocator: std.mem.Allocator, bytes: []u8, key_pair: Ed25519.KeyPair) Error!void {
    const head = try appimg.parse(bytes);
    const msg = try gate.signedMessage(head, bytes);
    const joined = try std.mem.concat(allocator, u8, &.{ msg.head, msg.tail });
    defer allocator.free(joined);
    const signature = key_pair.sign(joined, null) catch return Error.KeyRejected;
    @memcpy(bytes[Header.signature_offset..][0..appimg.Width.signature], &signature.toBytes());
}

/// Assemble and sign in one step. The caller owns the returned bytes.
pub fn pack(
    allocator: std.mem.Allocator,
    manifest: Manifest,
    code: []const u8,
    data: []const u8,
    key_pair: Ed25519.KeyPair,
) Error![]u8 {
    const bytes = try assemble(allocator, manifest, code, data);
    errdefer allocator.free(bytes);
    try sign(allocator, bytes, key_pair);
    return bytes;
}

/// The host's Ed25519 verify, in the gate's backend shape. Anything other
/// than a signature that verifies under `public_key` is `.bad`.
pub fn verifyStd(
    ctx: ?*anyopaque,
    msg: gate.Message,
    signature: *const [appimg.Width.signature]u8,
    public_key: *const [gate.pubkey_bytes]u8,
) gate.Verdict {
    _ = ctx;
    const key = Ed25519.PublicKey.fromBytes(public_key.*) catch return .bad;
    var verifier = Ed25519.Signature.fromBytes(signature.*).verifier(key) catch return .bad;
    verifier.update(msg.head);
    verifier.update(msg.tail);
    verifier.verify() catch return .bad;
    return .good;
}

/// A gate backend that checks signatures with `verifyStd`.
pub fn hostBackend(public_key: *const [gate.pubkey_bytes]u8, granted: u32) gate.Backend {
    return .{ .verify = verifyStd, .public_key = public_key, .granted = granted };
}
