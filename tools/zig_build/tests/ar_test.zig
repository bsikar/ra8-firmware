//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the static-archive reader. Every archive here is synthesised
//! byte by byte, so what the reader concludes is pinned from any host: the
//! point of reading an archive before linking it is to have evidence that does
//! not depend on the machine that produced it.

const std = @import("std");
const ar = @import("ar");

const Archive = struct {
    bytes: std.ArrayListUnmanaged(u8) = .empty,
    allocator: std.mem.Allocator,

    fn init(allocator: std.mem.Allocator) !Archive {
        var self: Archive = .{ .allocator = allocator };
        try self.bytes.appendSlice(allocator, ar.magic);
        return self;
    }

    fn deinit(self: *Archive) void {
        self.bytes.deinit(self.allocator);
    }

    fn member(self: *Archive, name: []const u8, data: []const u8) !void {
        var header: [ar.member_header_len]u8 = [_]u8{' '} ** ar.member_header_len;
        @memcpy(header[0..name.len], name);
        var size_text: [10]u8 = undefined;
        const size = try std.fmt.bufPrint(&size_text, "{d}", .{data.len});
        @memcpy(header[48..][0..size.len], size);
        header[58] = 0x60;
        header[59] = '\n';
        try self.bytes.appendSlice(self.allocator, &header);
        try self.bytes.appendSlice(self.allocator, data);
        if (data.len % 2 == 1) try self.bytes.append(self.allocator, '\n');
    }
};

/// A 64-bit little-endian ELF header, which is all the reader looks at.
fn elfObject(machine: u16, class: u8) [64]u8 {
    var bytes: [64]u8 = [_]u8{0} ** 64;
    @memcpy(bytes[0..4], "\x7fELF");
    bytes[4] = class;
    bytes[5] = 1; // little endian
    bytes[6] = 1; // EV_CURRENT
    bytes[16] = 1; // ET_REL
    std.mem.writeInt(u16, bytes[18..20], machine, .little);
    return bytes;
}

/// A 64-bit Mach-O object header, optionally carrying a macOS platform stamp.
fn machoObject(allocator: std.mem.Allocator, cpu_type: i32, stamped: bool) ![]u8 {
    var words: std.ArrayListUnmanaged(u8) = .empty;
    const append = struct {
        fn word(list: *std.ArrayListUnmanaged(u8), alloc: std.mem.Allocator, value: u32) !void {
            var buffer: [4]u8 = undefined;
            std.mem.writeInt(u32, &buffer, value, .little);
            try list.appendSlice(alloc, &buffer);
        }
    }.word;

    try append(&words, allocator, 0xfeedfacf);
    try append(&words, allocator, @bitCast(cpu_type));
    try append(&words, allocator, 0); // cpusubtype
    try append(&words, allocator, 1); // MH_OBJECT
    try append(&words, allocator, if (stamped) 1 else 0); // ncmds
    try append(&words, allocator, if (stamped) 24 else 0); // sizeofcmds
    try append(&words, allocator, 0); // flags
    try append(&words, allocator, 0); // reserved
    if (stamped) {
        try append(&words, allocator, 0x32); // LC_BUILD_VERSION
        try append(&words, allocator, 24);
        try append(&words, allocator, 1); // PLATFORM_MACOS
        try append(&words, allocator, 15 << 16); // minos 15.0.0
        try append(&words, allocator, 0); // sdk
        try append(&words, allocator, 0); // ntools
    }
    return words.toOwnedSlice(allocator);
}

test "an archive of ELF objects reads as ELF for its architecture" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = elfObject(0xb7, 2);
    try archive.member("fixture.o/", &object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.elf, description.format);
    try std.testing.expectEqual(std.Target.Cpu.Arch.aarch64, description.arch.?);
}

test "an archive of Mach-O objects reads as Mach-O for its architecture" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = try machoObject(std.testing.allocator, 0x0100000c, true);
    defer std.testing.allocator.free(object);
    try archive.member("fixture.o/", object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.mach_o, description.format);
    try std.testing.expectEqual(std.Target.Cpu.Arch.aarch64, description.arch.?);
    try std.testing.expect(description.macos_platform);
}

test "the architecture comes from the object, not the format" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = try machoObject(std.testing.allocator, 0x01000007, true);
    defer std.testing.allocator.free(object);
    try archive.member("fixture.o/", object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(std.Target.Cpu.Arch.x86_64, description.arch.?);
}

test "an unstamped Mach-O object is still readable, just unstamped" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = try machoObject(std.testing.allocator, 0x0100000c, false);
    defer std.testing.allocator.free(object);
    try archive.member("fixture.o/", object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(std.Target.Cpu.Arch.aarch64, description.arch.?);
    try std.testing.expect(!description.macos_platform);
}

test "the System V symbol table and string table are skipped" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    try archive.member("/", "symbols");
    try archive.member("//", "long/names/\n");
    const object = elfObject(0x3e, 2);
    try archive.member("/0", &object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.elf, description.format);
    try std.testing.expectEqual(std.Target.Cpu.Arch.x86_64, description.arch.?);
}

test "the Apple symbol table is skipped" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    try archive.member("__.SYMDEF SORTED", "symbols!");
    const object = try machoObject(std.testing.allocator, 0x0100000c, true);
    defer std.testing.allocator.free(object);
    try archive.member("fixture.o", object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.mach_o, description.format);
}

test "a BSD long name is stripped off the member data" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const long_name = "a_very_long_member_name.o";
    const object = elfObject(0xb7, 2);
    var data: std.ArrayListUnmanaged(u8) = .empty;
    defer data.deinit(std.testing.allocator);
    try data.appendSlice(std.testing.allocator, long_name);
    try data.appendSlice(std.testing.allocator, &object);
    try archive.member("#1/25", data.items);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.elf, description.format);
    try std.testing.expectEqual(std.Target.Cpu.Arch.aarch64, description.arch.?);
}

test "a 32-bit or big-endian ELF is not translated to an architecture" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = elfObject(0x28, 1);
    try archive.member("fixture.o/", &object);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.elf, description.format);
    try std.testing.expect(description.arch == null);
}

test "bytes that are not an archive are refused" {
    try std.testing.expectError(error.NotArchive, ar.describe("not an archive at all"));
    try std.testing.expectError(error.TooShort, ar.describe("!<ar"));
}

test "a truncated member is refused rather than guessed at" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    try archive.member("fixture.o/", "0123456789");
    const cut = archive.bytes.items[0 .. archive.bytes.items.len - 4];
    try std.testing.expectError(error.TruncatedMember, ar.describe(cut));

    var short: std.ArrayListUnmanaged(u8) = .empty;
    defer short.deinit(std.testing.allocator);
    try short.appendSlice(std.testing.allocator, ar.magic);
    try short.appendSlice(std.testing.allocator, "too short for a header");
    try std.testing.expectError(error.TruncatedMember, ar.describe(short.items));
}

test "a member header without its terminator or size is malformed" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    const object = elfObject(0xb7, 2);
    try archive.member("fixture.o/", &object);
    archive.bytes.items[ar.magic.len + 58] = 'x';
    try std.testing.expectError(error.MalformedMember, ar.describe(archive.bytes.items));

    var sizeless = try Archive.init(std.testing.allocator);
    defer sizeless.deinit();
    try sizeless.member("fixture.o/", &object);
    @memcpy(sizeless.bytes.items[ar.magic.len + 48 ..][0..10], "not-a-size");
    try std.testing.expectError(error.MalformedMember, ar.describe(sizeless.bytes.items));
}

test "an archive of nothing but bookkeeping has no object to read" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    try archive.member("/", "symbols");
    try archive.member("//", "names\n");
    try std.testing.expectError(error.NoObjectMembers, ar.describe(archive.bytes.items));
}

test "an unrecognised member format is an answer, not an error" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    try archive.member("notes.txt/", "this is plain text, not an object\n");

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.unknown, description.format);
    try std.testing.expect(!description.suits(.aarch64, .macos));
}

test "an archive suits a target only when both format and architecture match" {
    const mach_arm: ar.Description = .{ .format = .mach_o, .arch = .aarch64, .macos_platform = true };
    try std.testing.expect(mach_arm.suits(.aarch64, .macos));
    try std.testing.expect(!mach_arm.suits(.x86_64, .macos));
    try std.testing.expect(!mach_arm.suits(.aarch64, .linux));

    const elf_arm: ar.Description = .{ .format = .elf, .arch = .aarch64 };
    try std.testing.expect(elf_arm.suits(.aarch64, .linux));
    try std.testing.expect(!elf_arm.suits(.aarch64, .macos));

    // This is the case the guard exists for: the archive `cargo` leaves in a
    // Linux target directory, offered to an `aarch64-macos` link.
    try std.testing.expect(!elf_arm.suits(.aarch64, .macos));

    const nameless: ar.Description = .{ .format = .mach_o, .arch = null };
    try std.testing.expect(!nameless.suits(.aarch64, .macos));
}

test "each host os maps to the object format its linker consumes" {
    try std.testing.expectEqual(ar.Format.mach_o, ar.expectedFormat(.macos));
    try std.testing.expectEqual(ar.Format.elf, ar.expectedFormat(.linux));
    try std.testing.expectEqual(ar.Format.coff, ar.expectedFormat(.windows));
    try std.testing.expectEqual(ar.Format.unknown, ar.expectedFormat(.freestanding));
}

test "every format names itself for a failure message" {
    for ([_]ar.Format{ .mach_o, .elf, .coff, .wasm, .unknown }) |format| {
        try std.testing.expect(format.label().len > 0);
    }
}

test "a fat Mach-O archive member reads as Mach-O with no single architecture" {
    var archive = try Archive.init(std.testing.allocator);
    defer archive.deinit();
    var fat: [32]u8 = [_]u8{0} ** 32;
    std.mem.writeInt(u32, fat[0..4], 0xcafebabe, .little);
    try archive.member("fixture.o/", &fat);

    const description = try ar.describe(archive.bytes.items);
    try std.testing.expectEqual(ar.Format.mach_o, description.format);
    try std.testing.expect(description.arch == null);
}
