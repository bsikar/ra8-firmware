//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the Mach-O reader. Every image here is synthesised byte by
//! byte, so the reader's behaviour is pinned from any host: the point of
//! reading the artifact back is to have evidence that does not depend on the
//! machine that produced it.

const std = @import("std");
// Reached through the archive reader's module: `macho.zig` is a file inside it,
// and a file can belong to only one module in a compilation.
const macho = @import("ar").macho;

const Builder = struct {
    bytes: std.ArrayListUnmanaged(u8) = .empty,
    allocator: std.mem.Allocator,
    command_count: u32 = 0,
    /// Where `LC_CODE_SIGNATURE` sits, so its offsets can be patched once the
    /// signature blob is appended.
    signature_command: ?usize = null,

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

    /// `LC_CODE_SIGNATURE`, pointing at a region the caller appends later with
    /// `appendSignature`. The offset is patched by `finish` once the load
    /// commands have stopped moving.
    fn codeSignature(self: *Builder) !void {
        try self.append(macho.lc_code_signature);
        try self.append(16);
        try self.append(0); // dataoff, patched by finishSigned()
        try self.append(0); // datasize, patched by finishSigned()
        self.signature_command = self.bytes.items.len - 16;
        self.command_count += 1;
    }

    fn appendBig(self: *Builder, value: u32) !void {
        var word: [4]u8 = undefined;
        std.mem.writeInt(u32, &word, value, .big);
        try self.bytes.appendSlice(self.allocator, &word);
    }

    /// Close the header, then append an embedded signature super-blob holding
    /// one code directory, and point `LC_CODE_SIGNATURE` at it.
    ///
    /// `code_limit_delta` shifts the directory's `codeLimit` away from the real
    /// start of the blob, which is how an image edited after the link is
    /// synthesised.
    fn finishSigned(self: *Builder, options: SignatureOptions) ![]const u8 {
        _ = self.finish();
        const command = self.signature_command.?;
        const blob_start: u32 = @intCast(self.bytes.items.len);

        const identifier = options.identifier;
        // magic, length, version, flags, hashOffset, identOffset,
        // nSpecialSlots, nCodeSlots, codeLimit, then four byte-wide fields.
        const directory_fixed: u32 = 40;
        const directory_length: u32 = directory_fixed + @as(u32, @intCast(identifier.len)) + 1;
        const super_length: u32 = 12 + 8 + directory_length;
        const directory_offset: u32 = 20; // super-blob header plus one index entry

        try self.appendBig(options.super_magic);
        try self.appendBig(super_length);
        try self.appendBig(1); // one slot
        try self.appendBig(options.slot_type);
        try self.appendBig(directory_offset);

        try self.appendBig(options.directory_magic);
        try self.appendBig(directory_length);
        try self.appendBig(0x20400); // version
        try self.appendBig(options.flags);
        try self.appendBig(directory_fixed); // hashOffset, unread here
        try self.appendBig(directory_fixed); // identOffset
        try self.appendBig(0); // nSpecialSlots
        try self.appendBig(0); // nCodeSlots
        try self.appendBig(blob_start +% options.code_limit_delta);
        try self.bytes.append(self.allocator, 32); // hashSize
        try self.bytes.append(self.allocator, 2); // hashType, SHA-256
        try self.bytes.append(self.allocator, 1); // platform
        try self.bytes.append(self.allocator, 14); // pageSize, 16 KiB
        try self.bytes.appendSlice(self.allocator, identifier);
        try self.bytes.append(self.allocator, 0);

        self.u32At(command + 8, blob_start);
        self.u32At(command + 12, @intCast(self.bytes.items.len - blob_start));
        return self.bytes.items;
    }

    fn finish(self: *Builder) []const u8 {
        self.u32At(16, self.command_count);
        self.u32At(20, @intCast(self.bytes.items.len - 32));
        return self.bytes.items;
    }
};

const SignatureOptions = struct {
    identifier: []const u8 = "image_pyramid",
    /// Ad-hoc plus linker-signed, which is what Zig's Mach-O linker writes.
    flags: u32 = macho.cs_flag_adhoc | macho.cs_flag_linker_signed,
    code_limit_delta: u32 = 0,
    super_magic: u32 = macho.cs_magic_embedded_signature,
    directory_magic: u32 = macho.cs_magic_code_directory,
    slot_type: u32 = macho.cs_slot_code_directory,
};

/// The shape Zig emits for an arm64 host build: a signed arm64 macOS image
/// linking the system libSystem.
fn signedArm64Binary(
    allocator: std.mem.Allocator,
    builder: *Builder,
    options: SignatureOptions,
) ![]const u8 {
    builder.* = Builder.init(allocator);
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    try builder.loadDylib(macho.system_libsystem);
    try builder.codeSignature();
    return builder.finishSigned(options);
}

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

test "a linker-signed arm64 image reports its identifier, flags, and coverage" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{});
    defer builder.deinit();

    const signature = try macho.readSignature(bytes);
    try std.testing.expect(signature.isAdhoc());
    try std.testing.expect(signature.isLinkerSigned());
    try std.testing.expect(signature.coversImage());
    try std.testing.expectEqualStrings("image_pyramid", signature.identifier);
    try std.testing.expectEqual(@as(u32, 0x20400), signature.version);
    try std.testing.expectEqual(@as(u8, 32), signature.hash_size);
    try std.testing.expectEqual(@as(u8, 2), signature.hash_type);
    try std.testing.expectEqual(@as(u8, 14), signature.page_size_log2);
}

test "the command and the blob agree on where the signature lives" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{});
    defer builder.deinit();

    const image = try macho.read(bytes);
    const region = image.code_signature.?;
    const signature = try macho.readSignature(bytes);
    try std.testing.expectEqual(region.data_offset, signature.region.data_offset);
    try std.testing.expectEqual(region.data_offset, signature.code_limit);
    // The signature is the tail of the file: nothing follows it.
    try std.testing.expectEqual(bytes.len, region.data_offset + region.data_size);
}

test "an unsigned image is reported as missing a signature, not as unreadable" {
    var builder: Builder = undefined;
    const bytes = try arm64Binary(std.testing.allocator, &builder);
    defer builder.deinit();

    // The image itself still reads: this is a link that succeeded and produced
    // a binary arm64 macOS will kill at exec.
    const image = try macho.read(bytes);
    try std.testing.expect(image.links_system_libsystem);
    try std.testing.expect(image.code_signature == null);
    try std.testing.expectError(error.MissingCodeSignature, macho.readSignature(bytes));
}

test "a signature that stops short of its own blob is not covering the image" {
    var builder: Builder = undefined;
    // Something was inserted after the link: the signature covers 64 bytes
    // fewer than the image in front of it.
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .code_limit_delta = @as(u32, 0) -% 64 });
    defer builder.deinit();

    const signature = try macho.readSignature(bytes);
    try std.testing.expect(!signature.coversImage());
    try std.testing.expectEqual(signature.region.data_offset - 64, signature.code_limit);
}

test "a signature claiming more bytes than precede it is not covering the image either" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .code_limit_delta = 128 });
    defer builder.deinit();

    const signature = try macho.readSignature(bytes);
    try std.testing.expect(!signature.coversImage());
}

test "an identity-signed image is neither ad-hoc nor linker-signed" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .flags = 0 });
    defer builder.deinit();

    const signature = try macho.readSignature(bytes);
    try std.testing.expect(!signature.isAdhoc());
    try std.testing.expect(!signature.isLinkerSigned());
    // Still a readable, covering signature: the flags are a separate question
    // from whether the blob parses.
    try std.testing.expect(signature.coversImage());
}

test "a blob that is not an embedded signature is named as such" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .super_magic = 0xfade0b01 });
    defer builder.deinit();

    try std.testing.expectError(error.NotEmbeddedSignature, macho.readSignature(bytes));
}

test "an embedded signature with no code directory slot is distinct from a malformed one" {
    var builder: Builder = undefined;
    // A requirements slot in place of the code directory: the super-blob is
    // well formed, it simply says nothing about what range is covered.
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .slot_type = 2 });
    defer builder.deinit();

    try std.testing.expectError(error.NoCodeDirectory, macho.readSignature(bytes));
}

test "a code directory with the wrong magic is malformed" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{ .directory_magic = 0xfade0c01 });
    defer builder.deinit();

    try std.testing.expectError(error.MalformedSignatureBlob, macho.readSignature(bytes));
}

test "a signature command pointing past the end of the file is out of bounds" {
    var builder: Builder = undefined;
    const bytes = try signedArm64Binary(std.testing.allocator, &builder, .{});
    defer builder.deinit();

    const image = try macho.read(bytes);
    const mutable = @constCast(bytes);
    // Push dataoff beyond the file; the load command is still well formed.
    const command_offset = std.mem.indexOfPos(u8, mutable, 32, &.{ 0x1d, 0, 0, 0 }).?;
    std.mem.writeInt(u32, mutable[command_offset + 8 ..][0..4], @intCast(bytes.len + 4096), .little);
    try std.testing.expect(image.code_signature != null);
    try std.testing.expectError(error.SignatureOutOfBounds, macho.readSignature(mutable));
}

test "an empty signature region is out of bounds rather than a parse of zero bytes" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.buildVersion(macho.platform_macos, 26 << 16);
    try builder.codeSignature();
    const bytes = builder.finish();
    // finish() leaves the command's offset and size at zero, which is what a
    // stripped signature looks like.
    try std.testing.expectError(error.SignatureOutOfBounds, macho.readSignature(bytes));
}

test "a truncated load command is still rejected before any signature reading" {
    var builder = Builder.init(std.testing.allocator);
    defer builder.deinit();
    try builder.header(macho.magic_64_le, macho.cpu_type_arm64);
    try builder.append(macho.lc_code_signature);
    try builder.append(12); // too small to hold dataoff and datasize
    try builder.append(0);
    builder.command_count += 1;
    try std.testing.expectError(error.MalformedLoadCommand, macho.readSignature(builder.finish()));
}
