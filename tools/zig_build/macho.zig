//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Minimal Mach-O reader for checking what a host build actually produced (#899).
//!
//! The #899 rule is a claim about linkage: on an arm64 Mac the build must come
//! out as a native arm64 Mach-O, carrying the host's deployment target, linked
//! against the system `libSystem`. Until now the only evidence for that was
//! `zig build` exiting zero, which says the link succeeded but nothing about
//! what it produced. A build that quietly picked Zig's default macOS floor
//! instead of the host's version (the defect fixed for the pinned query) still
//! exits zero.
//!
//! So this reads the load commands back out of the emitted binary. It is a
//! deliberately small reader: the Mach-O header, `LC_BUILD_VERSION` (and the
//! older `LC_VERSION_MIN_MACOSX`), the `LC_LOAD_DYLIB` names, and the
//! `LC_CODE_SIGNATURE` blob. It parses a byte slice and allocates nothing, so
//! every branch is unit tested against synthesised images rather than needing
//! a Mac or `otool`.
//!
//! The signature is not a detail on arm64. Apple silicon refuses to execute an
//! unsigned Mach-O: the kernel kills the process at exec with `Killed: 9` and
//! no diagnostic, so a host binary that links perfectly can still be
//! unrunnable, which is the #899 failure shape exactly. Zig's own Mach-O
//! linker writes an ad-hoc signature, so the check is that what came out still
//! carries one and that it still covers the whole image.

const std = @import("std");

pub const magic_64_le: u32 = 0xfeedfacf;
pub const magic_32_le: u32 = 0xfeedface;
/// A universal ("fat") archive. Big-endian magic, so it reads as this value
/// whichever way round the host is.
pub const magic_fat: u32 = 0xcafebabe;
pub const magic_fat_swapped: u32 = 0xbebafeca;

pub const cpu_type_arm64: i32 = 0x0100000c;
pub const cpu_type_x86_64: i32 = 0x01000007;

pub const lc_load_dylib: u32 = 0x0c;
pub const lc_code_signature: u32 = 0x1d;
pub const lc_version_min_macosx: u32 = 0x24;
pub const lc_build_version: u32 = 0x32;

pub const platform_macos: u32 = 1;

pub const system_libsystem = "/usr/lib/libSystem.B.dylib";

/// Code-signing blob magics. Every blob in a signature is big-endian,
/// whichever way round the image itself is.
pub const cs_magic_embedded_signature: u32 = 0xfade0cc0;
pub const cs_magic_code_directory: u32 = 0xfade0c02;
/// The slot in the embedded super-blob that holds the code directory.
pub const cs_slot_code_directory: u32 = 0;

/// Signed with no identity: the signature vouches for the bytes, not for who
/// produced them. This is what a linker writes, and what arm64 macOS needs to
/// let the image run at all.
pub const cs_flag_adhoc: u32 = 0x0000_0002;
/// The signature was written by the linker rather than by `codesign`.
pub const cs_flag_linker_signed: u32 = 0x0002_0000;

pub const ReadError = error{
    /// Fewer bytes than a Mach-O header.
    TooShort,
    /// The magic is not a Mach-O magic at all.
    NotMachO,
    /// A universal archive rather than a single-architecture image.
    FatBinary,
    /// A 32-bit Mach-O. Nothing here targets one.
    NotSixtyFourBit,
    /// The header's load-command region runs past the end of the file.
    TruncatedLoadCommands,
    /// A load command declares a size that cannot be walked.
    MalformedLoadCommand,
};

pub const SignatureError = error{
    /// The image carries no `LC_CODE_SIGNATURE` at all.
    MissingCodeSignature,
    /// The command points outside the file.
    SignatureOutOfBounds,
    /// The blob at that offset is not an embedded signature super-blob.
    NotEmbeddedSignature,
    /// The super-blob's own lengths or slot table cannot be walked.
    MalformedSignatureBlob,
    /// An embedded signature with no code directory in it. Nothing then states
    /// what range of bytes the signature covers.
    NoCodeDirectory,
};

/// What the header and load commands say about an image.
pub const Image = struct {
    cpu_type: i32,
    cpu_subtype: i32,
    file_type: u32,
    command_count: u32,
    /// The platform from `LC_BUILD_VERSION`, when the image carries one.
    platform: ?u32 = null,
    /// The minimum OS version the image is stamped with, from either
    /// `LC_BUILD_VERSION` or the older `LC_VERSION_MIN_MACOSX`.
    minimum_os: ?std.SemanticVersion = null,
    /// How many `LC_LOAD_DYLIB` commands the image carries.
    dylib_count: usize = 0,
    /// Whether one of them is the system `libSystem`.
    links_system_libsystem: bool = false,
    /// Where `LC_CODE_SIGNATURE` says the signature lives, when the image
    /// carries one. The contents are read separately by `readSignature`.
    code_signature: ?SignatureRegion = null,

    /// The architecture this image is for, in Zig's spelling, when it is one we
    /// name. `null` means a cpu type this reader does not translate, which is
    /// still a readable image, just not one of ours.
    pub fn arch(self: Image) ?std.Target.Cpu.Arch {
        return switch (self.cpu_type) {
            cpu_type_arm64 => .aarch64,
            cpu_type_x86_64 => .x86_64,
            else => null,
        };
    }

    pub fn isMacosPlatform(self: Image) bool {
        return self.platform == platform_macos;
    }
};

/// Decode a packed Mach-O version word (`xxxx.yy.zz`, 16.8.8 bits).
pub fn decodeVersion(packed_version: u32) std.SemanticVersion {
    return .{
        .major = packed_version >> 16,
        .minor = (packed_version >> 8) & 0xff,
        .patch = packed_version & 0xff,
    };
}

fn readU32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

/// Does `name` name the system libSystem, however the linker spelled the path?
pub fn isSystemLibSystem(name: []const u8) bool {
    if (std.mem.eql(u8, name, system_libsystem)) return true;
    // Some link paths carry a prefix (an SDK root, `@rpath`). The basename is
    // the part that identifies the library.
    return std.mem.endsWith(u8, name, "/libSystem.B.dylib");
}

/// Read the header and load commands of a single-architecture Mach-O image.
pub fn read(bytes: []const u8) ReadError!Image {
    if (bytes.len < 4) return error.TooShort;
    const magic = readU32(bytes, 0);
    switch (magic) {
        magic_fat, magic_fat_swapped => return error.FatBinary,
        magic_32_le, @byteSwap(magic_32_le) => return error.NotSixtyFourBit,
        magic_64_le => {},
        else => return error.NotMachO,
    }
    // A 64-bit header is 32 bytes: magic, cputype, cpusubtype, filetype,
    // ncmds, sizeofcmds, flags, reserved.
    if (bytes.len < 32) return error.TooShort;

    var image: Image = .{
        .cpu_type = @bitCast(readU32(bytes, 4)),
        .cpu_subtype = @bitCast(readU32(bytes, 8)),
        .file_type = readU32(bytes, 12),
        .command_count = readU32(bytes, 16),
    };
    const commands_size = readU32(bytes, 20);

    const commands_end = 32 + @as(usize, commands_size);
    if (commands_end > bytes.len) return error.TruncatedLoadCommands;

    var offset: usize = 32;
    var seen: u32 = 0;
    while (seen < image.command_count) : (seen += 1) {
        if (offset + 8 > commands_end) return error.TruncatedLoadCommands;
        const command = readU32(bytes, offset);
        const size = readU32(bytes, offset + 4);
        // A command must at least hold its own header, must not run past the
        // declared region, and must move the cursor forward.
        if (size < 8 or offset + size > commands_end) return error.MalformedLoadCommand;

        switch (command) {
            lc_build_version => {
                if (size < 24) return error.MalformedLoadCommand;
                image.platform = readU32(bytes, offset + 8);
                image.minimum_os = decodeVersion(readU32(bytes, offset + 12));
            },
            lc_version_min_macosx => {
                if (size < 16) return error.MalformedLoadCommand;
                // Only fill in from the legacy command when the modern one has
                // not already spoken; LC_BUILD_VERSION is the better source.
                if (image.platform == null) image.platform = platform_macos;
                if (image.minimum_os == null) image.minimum_os = decodeVersion(readU32(bytes, offset + 8));
            },
            lc_code_signature => {
                if (size < 16) return error.MalformedLoadCommand;
                image.code_signature = .{
                    .data_offset = readU32(bytes, offset + 8),
                    .data_size = readU32(bytes, offset + 12),
                };
            },
            lc_load_dylib => {
                if (size < 24) return error.MalformedLoadCommand;
                image.dylib_count += 1;
                const name_offset = readU32(bytes, offset + 8);
                if (name_offset < 24 or name_offset >= size) return error.MalformedLoadCommand;
                const raw = bytes[offset + name_offset .. offset + size];
                const name = std.mem.sliceTo(raw, 0);
                if (isSystemLibSystem(name)) image.links_system_libsystem = true;
            },
            else => {},
        }
        offset += size;
    }
    return image;
}

/// Where `LC_CODE_SIGNATURE` places the signature inside the file.
pub const SignatureRegion = struct {
    data_offset: u32,
    data_size: u32,
};

/// What the code directory inside an embedded signature says.
pub const Signature = struct {
    region: SignatureRegion,
    /// The code directory's version word (0x20400 for what Zig writes).
    version: u32,
    flags: u32,
    /// The number of bytes of the image the signature covers. Everything from
    /// here to the end of the file is the signature itself.
    code_limit: u32,
    hash_size: u8,
    hash_type: u8,
    /// Page size as a power of two (14 for the 16 KiB pages arm64 uses).
    page_size_log2: u8,
    /// The signing identifier, which for a linker signature is the artifact
    /// name. Borrows from the image bytes.
    identifier: []const u8,

    pub fn isAdhoc(self: Signature) bool {
        return self.flags & cs_flag_adhoc != 0;
    }

    pub fn isLinkerSigned(self: Signature) bool {
        return self.flags & cs_flag_linker_signed != 0;
    }

    /// Does the signature cover every byte in front of it?
    ///
    /// A signature vouches for bytes 0..code_limit, and the signature blob
    /// itself is what follows. So in an image nobody has touched since the
    /// link, `code_limit` is exactly where the blob starts. When they drift
    /// apart something was inserted, stripped, or appended afterwards, and
    /// macOS rejects the signature at exec rather than reporting the edit.
    pub fn coversImage(self: Signature) bool {
        return self.code_limit == self.region.data_offset;
    }
};

fn readU32Big(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .big);
}

/// Read the code directory out of an image's embedded signature.
///
/// Every structure here is big-endian and the offsets inside the super-blob
/// are relative to the blob, not to the file, which is why this is a separate
/// walk rather than another arm of the load-command switch.
pub fn readSignature(bytes: []const u8) (ReadError || SignatureError)!Signature {
    const image = try read(bytes);
    const region = image.code_signature orelse return error.MissingCodeSignature;

    const start: usize = region.data_offset;
    const end = start + @as(usize, region.data_size);
    if (region.data_size < 12 or end > bytes.len) return error.SignatureOutOfBounds;
    const blob = bytes[start..end];

    if (readU32Big(blob, 0) != cs_magic_embedded_signature) return error.NotEmbeddedSignature;
    const super_length = readU32Big(blob, 4);
    if (super_length < 12 or super_length > blob.len) return error.MalformedSignatureBlob;
    const count = readU32Big(blob, 8);

    var index: u32 = 0;
    while (index < count) : (index += 1) {
        const entry = 12 + @as(usize, index) * 8;
        if (entry + 8 > super_length) return error.MalformedSignatureBlob;
        const slot_type = readU32Big(blob, entry);
        const slot_offset = readU32Big(blob, entry + 4);
        if (slot_type != cs_slot_code_directory) continue;

        // magic, length, version, flags, hashOffset, identOffset,
        // nSpecialSlots, nCodeSlots, codeLimit, then the four byte-wide
        // fields: hashSize, hashType, platform, pageSize.
        const directory_fixed = 40;
        if (slot_offset + directory_fixed > super_length) return error.MalformedSignatureBlob;
        if (readU32Big(blob, slot_offset) != cs_magic_code_directory) return error.MalformedSignatureBlob;
        const directory_length = readU32Big(blob, slot_offset + 4);
        if (directory_length < directory_fixed or slot_offset + directory_length > super_length) {
            return error.MalformedSignatureBlob;
        }

        const identifier_offset = readU32Big(blob, slot_offset + 20);
        if (identifier_offset < directory_fixed or identifier_offset >= directory_length) {
            return error.MalformedSignatureBlob;
        }
        const identifier_start = slot_offset + identifier_offset;
        const identifier = std.mem.sliceTo(blob[identifier_start .. slot_offset + directory_length], 0);

        return .{
            .region = region,
            .version = readU32Big(blob, slot_offset + 8),
            .flags = readU32Big(blob, slot_offset + 12),
            .code_limit = readU32Big(blob, slot_offset + 32),
            .hash_size = blob[slot_offset + 36],
            .hash_type = blob[slot_offset + 37],
            .page_size_log2 = blob[slot_offset + 39],
            .identifier = identifier,
        };
    }
    return error.NoCodeDirectory;
}

/// Collect the `LC_LOAD_DYLIB` names into a caller-owned buffer, so a failing
/// check can print what the image actually links instead of only that the one
/// it wanted is absent. The returned slices borrow from `bytes`. Names past the
/// end of `out` are dropped; the count in `Image.dylib_count` is the true one.
pub fn dylibNames(bytes: []const u8, out: [][]const u8) ReadError![][]const u8 {
    const image = try read(bytes);
    var written: usize = 0;
    var offset: usize = 32;
    var seen: u32 = 0;
    while (seen < image.command_count and written < out.len) : (seen += 1) {
        const command = readU32(bytes, offset);
        const size = readU32(bytes, offset + 4);
        if (command == lc_load_dylib) {
            const name_offset = readU32(bytes, offset + 8);
            out[written] = std.mem.sliceTo(bytes[offset + name_offset .. offset + size], 0);
            written += 1;
        }
        offset += size;
    }
    return out[0..written];
}
