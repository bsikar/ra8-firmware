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
    /// Legacy format revision without icon metadata (v1).
    pub const version_v1: u32 = 0x00000001;
    /// Format revision with icon metadata (v2).
    pub const version_v2: u32 = 0x00000002;
    /// Header revision this implementation reads by default.
    pub const version: u32 = version_v2;
    /// Syscall-table generation the running firmware publishes.
    pub const api_version_current: u32 = 0x00000002;
    /// Per-segment byte cap: the Non-Secure region a module can be given.
    pub const segment_max: u32 = 0x00100000;
    /// Smallest usable module stack, bytes.
    pub const stack_min: u32 = 0x00000400;

    /// Fixed icon dimensions and 2-bit grayscale packed size (v2).
    pub const icon_width: u32 = 16;
    pub const icon_height: u32 = 16;
    pub const icon_bytes: u32 = 64;

    /// Header on-disk byte sizes.
    pub const header_bytes_v1: usize = 160;
    pub const header_bytes_v2: usize = 168;
};

/// What a module declares it needs, one bit per host resource.
pub const Capability = @import("api/abi.zig").Capability;

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
/// `extern` pins the C layout the on-disk format depends on: ten 32-bit
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
    icon_offset: u32,
    icon_length: u32,
    signature: [Width.signature]u8,

    /// Byte offset of `signature` in format v1 (on disk).
    pub const signature_offset_v1: usize = 96;
    /// Byte offset of `signature` in format v2 (on disk).
    pub const signature_offset_v2: usize = 104;
    /// Byte offset of `signature`, which is where the signed prefix ends.
    pub const signature_offset: usize = signature_offset_v2;

    /// On-disk header size for this header's format version.
    pub fn headerSize(self: Header) usize {
        return if (self.format_version == Format.version_v1)
            Format.header_bytes_v1
        else
            Format.header_bytes_v2;
    }

    /// On-disk signature offset for this header's format version.
    pub fn signatureOffset(self: Header) usize {
        return if (self.format_version == Format.version_v1)
            signature_offset_v1
        else
            signature_offset_v2;
    }

    /// Declared payload length: the module image that follows the header.
    pub fn payloadLen(self: Header) usize {
        const code_data = @as(usize, self.code_size) + @as(usize, self.data_size);
        if (self.icon_length > 0) {
            return @max(code_data, @as(usize, self.icon_offset) + @as(usize, self.icon_length));
        }
        return code_data;
    }

    /// Total declared file length: header plus payload.
    pub fn imageLen(self: Header) usize {
        return self.headerSize() + self.payloadLen();
    }
};

comptime {
    const words = 10 * @sizeOf(u32);
    const expected = words + Width.app_id + Width.display_name + Width.signature;
    std.debug.assert(@sizeOf(Header) == expected);
    std.debug.assert(@sizeOf(Header) == Format.header_bytes_v2);
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
    if (header.format_version != Format.version_v1 and header.format_version != Format.version_v2) return Error.Validation;
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

    if (header.format_version == Format.version_v1) {
        if (header.icon_offset != 0 or header.icon_length != 0) return Error.Validation;
    } else {
        if (header.icon_length == 0) {
            if (header.icon_offset != 0) return Error.Validation;
        } else {
            if (header.icon_length != Format.icon_bytes) return Error.OutOfRange;
            const code_data = @as(u64, header.code_size) + @as(u64, header.data_size);
            if (header.icon_offset < code_data) return Error.OutOfRange;
            const icon_end = @as(u64, header.icon_offset) + @as(u64, header.icon_length);
            if (icon_end > std.math.maxInt(u32)) return Error.OutOfRange;
        }
    }

    if (len < header.imageLen()) return Error.ShortImage;
}

/// Read and validate a header from the head of a file image.
///
/// The candidate is proven before it is returned, so a caller can never hold a
/// half-parsed header: on any refusal there is no value to hold at all.
pub fn parse(bytes: []const u8) Error!Header {
    if (bytes.len < Format.header_bytes_v1) return Error.ShortImage;

    const magic = std.mem.readInt(u32, bytes[0..4], .little);
    if (magic != Format.magic) return Error.Validation;

    const version = std.mem.readInt(u32, bytes[4..8], .little);
    if (version != Format.version_v1 and version != Format.version_v2) return Error.Validation;

    var candidate: Header = undefined;
    if (version == Format.version_v1) {
        @memcpy(std.mem.asBytes(&candidate)[0..Header.signature_offset_v1], bytes[0..Header.signature_offset_v1]);
        candidate.icon_offset = 0;
        candidate.icon_length = 0;
        @memcpy(&candidate.signature, bytes[Header.signature_offset_v1..Format.header_bytes_v1]);
    } else {
        if (bytes.len < Format.header_bytes_v2) return Error.ShortImage;
        @memcpy(std.mem.asBytes(&candidate), bytes[0..Format.header_bytes_v2]);
    }

    try checkIdentity(candidate);
    try checkSizes(candidate, bytes.len);
    return candidate;
}

/// The leading signed run: the header up to where `signature` begins.
pub fn signedSpan(header: Header, len: usize) Error!Span {
    if (len < header.headerSize()) return Error.ShortImage;
    return .{ .offset = 0, .length = @intCast(header.signatureOffset()) };
}

/// The payload run: everything after the header that the header declares.
pub fn payloadSpan(header: Header, len: usize) Error!Span {
    if (len < header.imageLen()) return Error.ShortImage;
    return .{
        .offset = @intCast(header.headerSize()),
        .length = @intCast(header.payloadLen()),
    };
}

/// Report the byte range the app icon occupies within the `.ra8app` file.
pub fn iconSpan(header: Header, len: usize) Error!Span {
    if (header.icon_length == 0) {
        return .{ .offset = 0, .length = 0 };
    }
    if (header.format_version < Format.version_v2) {
        return Error.Validation;
    }
    const h_size = header.headerSize();
    const abs_offset = h_size + @as(usize, header.icon_offset);
    const abs_end = abs_offset + @as(usize, header.icon_length);
    if (len < abs_end) return Error.ShortImage;
    return .{
        .offset = @intCast(abs_offset),
        .length = header.icon_length,
    };
}

/// Direct slice to the icon bytes, or null if no icon is declared.
pub fn iconSlice(header: Header, bytes: []const u8) Error!?[]const u8 {
    if (header.icon_length == 0) return null;
    const span = try iconSpan(header, bytes.len);
    return bytes[span.offset..][0..span.length];
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
