//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the Mach-O reader. Every image here is synthesised byte by
//! byte, so the reader's behaviour is pinned from any host: the point of
//! reading the artifact back is to have evidence that does not depend on the
//! machine that produced it.

const std = @import("std");
const macho = @import("macho");

const Builder = struct {
    bytes: std.ArrayListUnmanaged(u8) = .empty,
    allocator: std.mem.Allocator,
    command_count: u32 = 0,

    fn init(allocator: std.mem.Allocator) Builder {
        return .{ .allocator = allocator };
    }

    fn deinit(self: *Builder) void {
        self.bytes.deinit(self.allocator);
    }

    fn u32At(self: *Builder, offset: usize, value: u32) void {
        std.mem.writeInt(u32, self.bytes.items[offset..][0..4], value, .little);
    }

    fn append(self: *Builder, value: u32) !void {
        var word: [4]u8 = undefined;
        std.mem.writeInt(u32, &word, value, .little);
        try self.bytes.appendSlice(self.allocator, &word);
    }

    fn header(self: *Builder, magic: u32, cpu_type: i32) !void {
        try self.append(magic);
        try self.append(@bitCast(cpu_type));
        try self.append(0); // cpusubtype
        try self.append(2); // MH_EXECUTE
        try self.append(0); // ncmds, patched by finish()
        try self.append(0); // sizeofcmds, patched by finish()
        try self.append(0); // flags
        try self.append(0); // reserved
    }

    fn buildVersion(self: *Builder, platform: u32, minos: u32) !void {
        try self.append(macho.lc_build_version);
        try self.append(24);
        try self.append(platform);
        try self.append(minos);
        try self.append(0); // sdk
        try self.append(0); // ntools
        self.command_count += 1;
    }

    fn versionMinMacosx(self: *Builder, version: u32) !void {
        try self.append(macho.lc_version_min_macosx);
        try self.append(16);
        try self.append(version);
        try self.append(0); // sdk
        self.command_count += 1;
    }

    fn loadDylib(self: *Builder, name: []const u8) !void {
        const fixed = 24;
        const size: u32 = @intCast(std.mem.alignForward(usize, fixed + name.len + 1, 8));
        try self.append(macho.lc_load_dylib);
        try self.append(size);
        try self.append(fixed); // name offset
        try self.append(0); // timestamp
        try self.append(0); // current_version
        try self.append(0); // compatibility_version
        try self.bytes.appendSlice(self.allocator, name);
        try self.bytes.appendNTimes(self.allocator, 0, size - fixed - name.len);
        self.command_count += 1;
    }

    /// An unremarkable command the reader should walk past without comment.
    fn opaqueCommand(self: *Builder) !void {
        try self.append(0x19); // LC_SEGMENT_64, truncated on purpose
        try self.append(16);
        try self.append(0);
        try self.append(0);
        self.command_count += 1;
    }

    fn finish(self: *Builder) []const u8 {
        self.u32At(16, self.command_count);
        self.u32At(20, @intCast(self.bytes.items.len - 32));
        return self.bytes.items;
    }
};

fn arm64Binary(allocator: std.mem.Allocator, builder: *Builder) ![]const u8 {
    builder.* = Builder.init(allocator);
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    try builder.opaqueCommand();
    try builder.loadDylib(macho.system_libsystem);
    return builder.finish();
}

test "a native arm64 macOS image reads back as arm64, macOS, and libSystem-linked" {
    var builder: Builder = undefined;
    const bytes = try arm64Binary(std.testing.allocator, &builder);
    defer builder.deinit();

    const image = try macho.read(bytes);
    try std.testing.expectEqual(std.Target.Cpu.Arch.aarch64, image.arch().?);
    try std.testing.expect(image.isMacosPlatform());
    try std.testing.expectEqual(@as(u32, 26), image.minimum_os.?.major);
    try std.testing.expectEqual(@as(usize, 1), image.dylib_count);
    try std.testing.expect(image.links_system_libsystem);
}

test "the deployment target is read exactly, not rounded to the major version" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    // 15.4.1
    try builder.buildVersion(macho.platform_macos, (15 << 16) | (4 << 8) | 1);
    const image = try macho.read(builder.finish());
    const version = image.minimum_os.?;
    try std.testing.expectEqual(@as(u32, 15), version.major);
    try std.testing.expectEqual(@as(u32, 4), version.minor);
    try std.testing.expectEqual(@as(u32, 1), version.patch);
}

test "an x86_64 image is readable and reports the other architecture" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_x86_64);
    try builder.buildVersion(macho.platform_macos, 14 << 16);
    const image = try macho.read(builder.finish());
    try std.testing.expectEqual(std.Target.Cpu.Arch.x86_64, image.arch().?);
}

test "an unlisted cpu type is still a readable image, just not one we name" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, 0x0100000e);
    const image = try macho.read(builder.finish());
    try std.testing.expect(image.arch() == null);
}

test "an image without a build-version command reports no platform or minimum" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.loadDylib(macho.system_libsystem);
    const image = try macho.read(builder.finish());
    try std.testing.expect(image.platform == null);
    try std.testing.expect(image.minimum_os == null);
    try std.testing.expect(!image.isMacosPlatform());
}

test "the legacy LC_VERSION_MIN_MACOSX command is read when that is all there is" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.versionMinMacosx((12 << 16) | (3 << 8));
    const image = try macho.read(builder.finish());
    try std.testing.expect(image.isMacosPlatform());
    try std.testing.expectEqual(@as(u32, 12), image.minimum_os.?.major);
    try std.testing.expectEqual(@as(u32, 3), image.minimum_os.?.minor);
}

test "LC_BUILD_VERSION wins over a legacy command that follows it" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    try builder.versionMinMacosx(11 << 16);
    const image = try macho.read(builder.finish());
    try std.testing.expectEqual(@as(u32, 26), image.minimum_os.?.major);
}

test "an image linking something other than libSystem is not mistaken for one that does" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.loadDylib("/usr/lib/libc++.1.dylib");
    const image = try macho.read(builder.finish());
    try std.testing.expectEqual(@as(usize, 1), image.dylib_count);
    try std.testing.expect(!image.links_system_libsystem);
}

test "libSystem is recognised under an SDK prefix but a lookalike name is not" {
    try std.testing.expect(macho.isSystemLibSystem("/usr/lib/libSystem.B.dylib"));
    try std.testing.expect(macho.isSystemLibSystem("/Some.sdk/usr/lib/libSystem.B.dylib"));
    try std.testing.expect(!macho.isSystemLibSystem("/usr/lib/libSystemish.B.dylib"));
    try std.testing.expect(!macho.isSystemLibSystem("/usr/lib/libSystem.dylib"));
}

test "dylib names come back in order for a failure message" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.loadDylib("/usr/lib/libc++.1.dylib");
    try builder.opaqueCommand();
    try builder.loadDylib(macho.system_libsystem);
    var buffer: [4][]const u8 = undefined;
    const names = try macho.dylibNames(builder.finish(), &buffer);
    try std.testing.expectEqual(@as(usize, 2), names.len);
    try std.testing.expectEqualStrings("/usr/lib/libc++.1.dylib", names[0]);
    try std.testing.expectEqualStrings(macho.system_libsystem, names[1]);
}

test "a universal archive is named as one instead of read as a single image" {
    var bytes: [32]u8 = @splat(0);
    std.mem.writeInt(u32, bytes[0..4], macho.magic_fat, .little);
    try std.testing.expectError(error.FatBinary, macho.read(&bytes));
    std.mem.writeInt(u32, bytes[0..4], macho.magic_fat_swapped, .little);
    try std.testing.expectError(error.FatBinary, macho.read(&bytes));
}

test "a 32-bit Mach-O, an ELF, and a short file each fail with their own error" {
    var bytes: [32]u8 = @splat(0);
    std.mem.writeInt(u32, bytes[0..4], macho.magic_32_le, .little);
    try std.testing.expectError(error.NotSixtyFourBit, macho.read(&bytes));

    const elf = [_]u8{ 0x7f, 'E', 'L', 'F' } ++ [_]u8{0} ** 28;
    try std.testing.expectError(error.NotMachO, macho.read(&elf));

    try std.testing.expectError(error.TooShort, macho.read(&[_]u8{ 0xcf, 0xfa }));
}

test "a header promising more load commands than the file holds is rejected" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    _ = builder.finish();
    // Claim one more command than was written.
    builder.u32At(16, builder.command_count + 1);
    try std.testing.expectError(error.TruncatedLoadCommands, macho.read(builder.bytes.items));
}

test "a load command with an impossible size is rejected rather than walked" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    _ = builder.finish();
    builder.u32At(36, 4); // cmdsize smaller than the command header
    try std.testing.expectError(error.MalformedLoadCommand, macho.read(builder.bytes.items));
}

test "a dylib command whose name offset points outside it is rejected" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.loadDylib(macho.system_libsystem);
    _ = builder.finish();
    builder.u32At(40, 4096); // name offset past the end of the command
    try std.testing.expectError(error.MalformedLoadCommand, macho.read(builder.bytes.items));
}

test "a truncated sizeofcmds region is rejected before any command is read" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    _ = builder.finish();
    builder.u32At(20, 4096); // sizeofcmds beyond the file
    try std.testing.expectError(error.TruncatedLoadCommands, macho.read(builder.bytes.items));
}
