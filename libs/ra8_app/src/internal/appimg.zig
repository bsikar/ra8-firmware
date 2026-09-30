//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `.ra8app` container format: the on-disk header, the proofs a candidate
//! must pass, and the two byte runs a signature covers. The format and nothing
//! more, so there is no allocation, no I/O and no cryptography here. The
//! admission decision that spends a signature lives in `appimg_verify.zig`.

const std = @import("std");

/// Fixed byte-widths of the header's sized fields.
pub const Width = struct {
    /// Ed25519 raw `R || S` signature length.
    pub const signature: usize = 64;
    /// `app_id` field width, including the NUL.
    pub const app_id: usize = 32;
    /// `display_name` field width, including the NUL.
    pub const display_name: usize = 32;
};

/// Container magic, format revision and the loadable-size caps.
pub const Format = struct {
    /// ASCII "RA8A", the marker at the head of every `.ra8app` file.
    pub const magic: u32 = 0x52413841;
    /// Header revision this implementation reads.
    pub const version: u32 = 0x00000001;
    /// Syscall-table generation the running firmware publishes.
    pub const api_version_current: u32 = 0x00000001;
    /// Per-segment byte cap: the Non-Secure region a module can be given.
    pub const segment_max: u32 = 0x00100000;
    /// Smallest usable module stack, bytes.
    pub const stack_min: u32 = 0x00000400;
};

/// What a module declares it needs, one bit per host resource.
pub const Capability = struct {
    pub const none: u32 = 0x00000000;
    pub const display: u32 = 0x00000001;
    pub const storage: u32 = 0x00000002;
    pub const network: u32 = 0x00000004;
    /// Mask of every bit this format revision defines.
    pub const known: u32 = 0x00000007;
};

/// Why a candidate container was refused.
///
/// The membrane maps these onto `ra8_err_t`; keeping them a Zig error set here
/// means the format code cannot accidentally return a success code on a path
/// that refused.
pub const Error = error{
    /// Magic, revision, capability bits or a text field is malformed.
    Validation,
    /// `min_api_version` is newer than this firmware publishes.
    Unsupported,
    /// A declared size or the entry offset is outside its permitted range.
    OutOfRange,
    /// The image is shorter than the header, or than the payload it declares.
    ShortImage,
};

/// The `.ra8app` file header, in wire order and wire layout.
///
/// `extern` pins the C layout the on-disk format depends on: eight 32-bit
/// words, the two fixed-width text fields, then the signature, with no
/// implicit padding on any little-endian target.
pub const Header = extern struct {
    magic: u32,
    format_version: u32,
    entry_offset: u32,
    code_size: u32,
    data_size: u32,
    stack_size: u32,
    min_api_version: u32,
    capabilities: u32,
    app_id: [Width.app_id]u8,
    display_name: [Width.display_name]u8,
    signature: [Width.signature]u8,

    /// Byte offset of `signature`, which is where the signed prefix ends.
    pub const signature_offset: usize = @offsetOf(Header, "signature");

    /// Declared payload length: the module image that follows the header.
    pub fn payloadLen(self: Header) usize {
        return @as(usize, self.code_size) + @as(usize, self.data_size);
    }

    /// Total declared file length: header plus payload.
    pub fn imageLen(self: Header) usize {
        return @sizeOf(Header) + self.payloadLen();
    }
};

comptime {
    const words = 8 * @sizeOf(u32);
    const expected = words + Width.app_id + Width.display_name + Width.signature;
    std.debug.assert(@sizeOf(Header) == expected);
    std.debug.assert(Header.signature_offset == words + Width.app_id + Width.display_name);
}

/// A byte range within the file, measured from its first byte.
///
/// An offset and a length rather than a pointer, so the signer and the
/// verifier can name the same region and a tool can print it.
pub const Span = struct {
    offset: u32 = 0,
    length: u32 = 0,
};

/// Whether a fixed-width text field terminates inside its width.
fn terminated(field: []const u8) bool {
    return std.mem.indexOfScalar(u8, field, 0) != null;
}

/// Prove the fixed content fields: magic, revision, capabilities, text, API.
fn checkIdentity(header: Header) Error!void {
    if (header.magic != Format.magic) return Error.Validation;
    if (header.format_version != Format.version) return Error.Validation;
    if ((header.capabilities & ~Capability.known) != 0) return Error.Validation;
    if (!terminated(&header.app_id)) return Error.Validation;
    if (!terminated(&header.display_name)) return Error.Validation;
    if (header.app_id[0] == 0) return Error.Validation;
    if (header.min_api_version > Format.api_version_current) return Error.Unsupported;
}

/// Prove the declared sizes against their caps and against the file length.
fn checkSizes(header: Header, len: usize) Error!void {
    if (header.code_size == 0) return Error.OutOfRange;
    if (header.code_size > Format.segment_max) return Error.OutOfRange;
    if (header.data_size > Format.segment_max) return Error.OutOfRange;
    if (header.stack_size < Format.stack_min) return Error.OutOfRange;
    if (header.stack_size > Format.segment_max) return Error.OutOfRange;
    if (header.entry_offset >= header.code_size) return Error.OutOfRange;
    if (len < header.imageLen()) return Error.ShortImage;
}

/// Read and validate a header from the head of a file image.
///
/// The candidate is proven before it is returned, so a caller can never hold a
/// half-parsed header: on any refusal there is no value to hold at all.
pub fn parse(bytes: []const u8) Error!Header {
    if (bytes.len < @sizeOf(Header)) return Error.ShortImage;

    var candidate: Header = undefined;
    @memcpy(std.mem.asBytes(&candidate), bytes[0..@sizeOf(Header)]);

    try checkIdentity(candidate);
    try checkSizes(candidate, bytes.len);
    return candidate;
}

/// The leading signed run: the header up to where `signature` begins.
pub fn signedSpan(len: usize) Error!Span {
    if (len < @sizeOf(Header)) return Error.ShortImage;
    return .{ .offset = 0, .length = @intCast(Header.signature_offset) };
}

/// The payload run: everything after the header that the header declares.
pub fn payloadSpan(header: Header, len: usize) Error!Span {
    if (len < header.imageLen()) return Error.ShortImage;
    return .{
        .offset = @intCast(@sizeOf(Header)),
        .length = @intCast(header.payloadLen()),
    };
}

/// Whether a host grant satisfies the manifest an image declares.
///
/// `null` means permitted. A grant carrying an undefined bit is itself
/// malformed and is refused before the comparison, so a host cannot widen the
/// format by handing over a bit this revision does not define.
pub fn capabilitiesPermitted(header: Header, granted: u32) ?CapabilityRefusal {
    if ((granted & ~Capability.known) != 0) return .malformed_grant;
    if ((header.capabilities & ~granted) != 0) return .withheld;
    return null;
}

/// Why a manifest failed against a grant.
pub const CapabilityRefusal = enum {
    /// The grant itself carries a bit this revision does not define.
    malformed_grant,
    /// The app declares a capability the host withheld.
    withheld,
};
