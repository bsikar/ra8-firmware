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
//! older `LC_VERSION_MIN_MACOSX`), and the `LC_LOAD_DYLIB` names. It parses a
//! byte slice and allocates nothing, so every branch is unit tested against
//! synthesised images rather than needing a Mac or `otool`.

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
pub const lc_version_min_macosx: u32 = 0x24;
pub const lc_build_version: u32 = 0x32;

pub const platform_macos: u32 = 1;

pub const system_libsystem = "/usr/lib/libSystem.B.dylib";

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
