//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Just enough of the `ar` static-archive format to say which object format
//! and architecture an archive holds.
//!
//! The host Zig roots link Rust archives that a separate `cargo` invocation
//! produced (`tests/rust_abi_fixture`, `tests/abi_chain_fixture`,
//! `apps/host/firmware_pipeline/zig`). `cargo` with no `--target` builds for
//! the machine it runs on, so the archive and the Zig target agree only as
//! long as nobody pins a target and nobody reuses a target directory a
//! different host filled in. When they disagree the link fails deep inside the
//! linker, naming a symbol rather than the mismatch, which is the single
//! reason those three roots are still outside the macOS gate (#899).
//!
//! Reading the archive answers that directly: an archive of ELF objects is not
//! a thing an `aarch64-macos` link can use, whatever the symbols look like.
//! The reader is allocation-free and works over a byte slice, so every case is
//! unit tested from any host.

const std = @import("std");

/// The Mach-O reader, re-exported. A file belongs to exactly one module per
/// compilation, so a test graph reaches `macho` through this module rather than
/// rooting `macho.zig` a second time.
pub const macho = @import("macho.zig");

pub const magic = "!<arch>\n";

/// A member header is a fixed 60 bytes: name, mtime, uid, gid, mode, size and
/// the `` `\n `` terminator.
pub const member_header_len = 60;

pub const ReadError = error{
    /// Fewer bytes than the archive magic.
    TooShort,
    /// The magic is not an `ar` archive at all.
    NotArchive,
    /// A member header or its data runs past the end of the archive.
    TruncatedMember,
    /// A member header's size field cannot be read, or the header lacks its
    /// terminator.
    MalformedMember,
    /// The archive holds nothing but bookkeeping members.
    NoObjectMembers,
};

/// The object formats worth telling apart here. Anything else is `unknown`,
/// which is still an answer: it is not the format the target needs.
pub const Format = enum {
    mach_o,
    elf,
    coff,
    wasm,
    unknown,

    pub fn label(self: Format) []const u8 {
        return switch (self) {
            .mach_o => "Mach-O",
            .elf => "ELF",
            .coff => "COFF/PE",
            .wasm => "WebAssembly",
            .unknown => "unrecognised",
        };
    }
};

/// What an object file says about itself.
pub const Description = struct {
    format: Format,
    /// `null` for an architecture this reader does not translate, which is a
    /// mismatch rather than a pass.
    arch: ?std.Target.Cpu.Arch = null,
    /// Only a Mach-O carries this, and only when it is stamped.
    macos_platform: bool = false,

    pub fn suits(self: Description, target_arch: std.Target.Cpu.Arch, target_os: std.Target.Os.Tag) bool {
        if (self.format != expectedFormat(target_os)) return false;
        const actual = self.arch orelse return false;
        return actual == target_arch;
    }
};

/// The object format a link for `os` consumes.
pub fn expectedFormat(os: std.Target.Os.Tag) Format {
    return switch (os) {
        .macos, .ios, .tvos, .watchos, .visionos => .mach_o,
        .linux, .freebsd, .openbsd, .netbsd, .dragonfly, .solaris, .illumos => .elf,
        .windows => .coff,
        .wasi, .freestanding => .unknown,
        else => .unknown,
    };
}

fn readU32(bytes: []const u8, offset: usize) u32 {
    return std.mem.readInt(u32, bytes[offset..][0..4], .little);
}

/// Which format an object file's leading bytes declare.
pub fn objectFormat(bytes: []const u8) Format {
    if (bytes.len >= 4) {
        if (std.mem.eql(u8, bytes[0..4], "\x7fELF")) return .elf;
        if (std.mem.eql(u8, bytes[0..4], "\x00asm")) return .wasm;
        switch (readU32(bytes, 0)) {
            macho.magic_64_le,
            macho.magic_32_le,
            @byteSwap(macho.magic_64_le),
            @byteSwap(macho.magic_32_le),
            macho.magic_fat,
            macho.magic_fat_swapped,
            => return .mach_o,
            else => {},
        }
    }
    // A COFF object leads with its machine word; a PE image leads with `MZ`.
    if (bytes.len >= 2) {
        if (std.mem.eql(u8, bytes[0..2], "MZ")) return .coff;
        const machine = std.mem.readInt(u16, bytes[0..2], .little);
        if (machine == 0x8664 or machine == 0xaa64 or machine == 0x014c) return .coff;
    }
    return .unknown;
}

/// The ELF `e_machine` values this reader translates. A 32-bit or big-endian
/// ELF is left as `null`: it is a mismatch either way.
fn elfArch(bytes: []const u8) ?std.Target.Cpu.Arch {
    if (bytes.len < 20) return null;
    if (bytes[4] != 2) return null; // ELFCLASS64
    if (bytes[5] != 1) return null; // ELFDATA2LSB
    return switch (std.mem.readInt(u16, bytes[18..20], .little)) {
        0x3e => .x86_64,
        0xb7 => .aarch64,
        0xf3 => .riscv64,
        else => null,
    };
}

/// Read one object file's format and architecture.
pub fn describeObject(bytes: []const u8) Description {
    const format = objectFormat(bytes);
    return switch (format) {
        .elf => .{ .format = .elf, .arch = elfArch(bytes) },
        .mach_o => blk: {
            const image = macho.read(bytes) catch break :blk Description{ .format = .mach_o };
            break :blk .{
                .format = .mach_o,
                .arch = image.arch(),
                .macos_platform = image.isMacosPlatform(),
            };
        },
        else => .{ .format = format },
    };
}

/// Is this member name bookkeeping rather than an object?
///
/// `/` and `/SYM64/` are the System V symbol tables, `//` the long-name string
/// table, and `__.SYMDEF` (optionally ` SORTED`) the BSD and Apple one.
fn isBookkeeping(name: []const u8) bool {
    if (std.mem.eql(u8, name, "/")) return true;
    if (std.mem.eql(u8, name, "//")) return true;
    if (std.mem.eql(u8, name, "/SYM64/")) return true;
    return std.mem.startsWith(u8, name, "__.SYMDEF");
}

/// The bytes of the first member that is not bookkeeping.
pub fn firstObject(bytes: []const u8) ReadError![]const u8 {
    if (bytes.len < magic.len) return error.TooShort;
    if (!std.mem.eql(u8, bytes[0..magic.len], magic)) return error.NotArchive;

    var offset: usize = magic.len;
    while (offset < bytes.len) {
        if (offset + member_header_len > bytes.len) return error.TruncatedMember;
        const header = bytes[offset..][0..member_header_len];
        if (header[58] != 0x60 or header[59] != '\n') return error.MalformedMember;

        const size = std.fmt.parseInt(
            usize,
            std.mem.trim(u8, header[48..58], " "),
            10,
        ) catch return error.MalformedMember;

        const data_start = offset + member_header_len;
        if (data_start + size > bytes.len) return error.TruncatedMember;
        var data = bytes[data_start .. data_start + size];

        // A raw name is padded with spaces; System V also terminates it with a
        // slash, and the BSD form stores a long name at the head of the data
        // after a `#1/<length>` marker.
        var name = std.mem.trimRight(u8, header[0..16], " ");
        if (std.mem.startsWith(u8, name, "#1/")) {
            const name_len = std.fmt.parseInt(usize, std.mem.trim(u8, name[3..], " "), 10) catch
                return error.MalformedMember;
            if (name_len > data.len) return error.MalformedMember;
            name = std.mem.sliceTo(data[0..name_len], 0);
            data = data[name_len..];
        } else if (name.len > 1 and std.mem.endsWith(u8, name, "/")) {
            name = name[0 .. name.len - 1];
        }

        if (!isBookkeeping(name)) return data;

        // Member data is padded to an even offset.
        offset = data_start + size + (size % 2);
    }
    return error.NoObjectMembers;
}

/// What the archive's first real member says it is.
pub fn describe(bytes: []const u8) ReadError!Description {
    return describeObject(try firstObject(bytes));
}
