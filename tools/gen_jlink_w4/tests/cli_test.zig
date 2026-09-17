//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Exit-status and stream tests for `gen_jlink_w4` (#858). Every case runs the
//! real CLI against a temporary directory, so the ordering the predecessor
//! inherited is pinned: the usage line, then the base address, then the option
//! scan, then the image read, then the vector table, and only then output.

const std = @import("std");
const cli = @import("cli");
const testing = std.testing;

const Result = struct {
    status: u8,
    out: []u8,
    err: []u8,

    fn deinit(self: Result) void {
        testing.allocator.free(self.out);
        testing.allocator.free(self.err);
    }
};

fn runIn(dir: std.fs.Dir, arguments: []const []const u8) !Result {
    var out = std.ArrayList(u8).init(testing.allocator);
    errdefer out.deinit();
    var err = std.ArrayList(u8).init(testing.allocator);
    errdefer err.deinit();
    const status = try cli.run(
        testing.allocator,
        arguments,
        .{ .dir = dir, .program_name = "scripts/gen/gen_jlink_w4.py" },
        .{ .out = out.writer().any(), .err = err.writer().any() },
    );
    return .{
        .status = status,
        .out = try out.toOwnedSlice(),
        .err = try err.toOwnedSlice(),
    };
}

/// A minimal but realistic image: MSP 0x20010000, reset handler 0x0200000D.
const vector_image = [_]u8{ 0x00, 0x00, 0x01, 0x20, 0x0D, 0x00, 0x00, 0x02 };

fn withImage(image: []const u8, arguments: []const []const u8) !Result {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.writeFile(.{ .sub_path = "firmware.bin", .data = image });
    return runIn(tmp.dir, arguments);
}

test "no arguments print the usage line and exit 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expectEqualStrings(
        "Usage: scripts/gen/gen_jlink_w4.py <binary> <base_addr_hex> [--device DEV]\n",
        result.err,
    );
}

test "one argument is still a usage error" {
    const result = try withImage(&vector_image, &.{"firmware.bin"});
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expect(std.mem.startsWith(u8, result.err, "Usage: "));
}

test "a well-formed run emits the whole script and exits 0" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x02000000" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqualStrings("", result.err);
    try testing.expectEqualStrings(
        \\device R7KA8D2KF_CPU0
        \\si SWD
        \\speed 1000
        \\connect
        \\halt
        \\w4 0x02000000 0x20010000
        \\w4 0x02000004 0x0200000D
        \\w4 0xE000EDF8 0x20010000
        \\w4 0xE000EDF4 0x00010011
        \\w4 0xE000EDF8 0x01000000
        \\w4 0xE000EDF4 0x00010010
        \\w4 0xE000EDF8 0x00000000
        \\w4 0xE000EDF4 0x00010014
        \\w4 0xE000EDF8 0x0200000C
        \\w4 0xE000EDF4 0x0001000F
        \\g
        \\q
        \\
    , result.out);
}

test "the data words are written in image order, one per word" {
    const image = [_]u8{ 0x00, 0x00, 0x01, 0x20, 0x0D, 0x00, 0x00, 0x02, 0xAA, 0xBB, 0xCC, 0xDD };
    const result = try withImage(&image, &.{ "firmware.bin", "0x02000000" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x02000008 0xDDCCBBAA\n") != null);
}

test "a short final word is padded with 0xFF" {
    const image = [_]u8{ 0x00, 0x00, 0x01, 0x20, 0x0D, 0x00, 0x00, 0x02, 0x01 };
    const result = try withImage(&image, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000008 0xFFFFFF01\n") != null);
}

test "padding can supply the reset handler itself" {
    const image = [_]u8{ 0x01, 0x02, 0x03, 0x04, 0x05 };
    const result = try withImage(&image, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0xE000EDF8 0xFFFFFF04\n") != null);
}

test "--device replaces the default device" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--device", "R7FA8D1BH" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "device R7FA8D1BH\n"));
}

test "the last --device wins" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--device", "A", "--device", "B" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "device B\n"));
}

test "a --device value is never read as another option" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--device", "--device" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "device --device\n"));
}

test "a trailing --device with no value is an unknown argument" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--device" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expectEqualStrings("Unknown arg: --device\n", result.err);
}

test "an unrecognised option exits 1 and names itself" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "-x" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("Unknown arg: -x\n", result.err);
}

test "a stray positional is an unknown argument too" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "extra" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("Unknown arg: extra\n", result.err);
}

test "--selftest is not a flag here, since the predecessor had none" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--selftest" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("Unknown arg: --selftest\n", result.err);
}

test "a malformed base address exits 1 before the option scan" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "zz", "-x" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expectEqualStrings(
        "gen_jlink_w4: invalid literal for int() with base 16: 'zz'\n",
        result.err,
    );
}

test "a malformed base address outranks a missing image" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{ "absent.bin", "0x" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expect(std.mem.indexOf(u8, result.err, "invalid literal") != null);
}

test "a missing image exits 1 with no partial script" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    const result = try runIn(tmp.dir, &.{ "absent.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expect(std.mem.startsWith(u8, result.err, "gen_jlink_w4: cannot read 'absent.bin': "));
}

test "a directory in place of the image exits 1" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makeDir("firmware.bin");
    const result = try runIn(tmp.dir, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expect(std.mem.indexOf(u8, result.err, "cannot read 'firmware.bin'") != null);
}

test "an empty image exits 1 where unpack_from raised at offset 0" {
    const result = try withImage("", &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expectEqualStrings(
        "gen_jlink_w4: unpack requires a buffer of at least 4 bytes for unpacking 4 bytes at offset 0 (actual buffer size is 0)\n",
        result.err,
    );
}

test "a four-byte image exits 1 where unpack_from raised at offset 4" {
    const result = try withImage(&[_]u8{ 1, 2, 3, 4 }, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expectEqualStrings("", result.out);
    try testing.expectEqualStrings(
        "gen_jlink_w4: unpack requires a buffer of at least 8 bytes for unpacking 4 bytes at offset 4 (actual buffer size is 4)\n",
        result.err,
    );
}

test "a one-byte image is padded to four and still too short" {
    const result = try withImage(&[_]u8{0x01}, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 1), result.status);
    try testing.expect(std.mem.indexOf(u8, result.err, "at offset 4 (actual buffer size is 4)") != null);
}

test "a five-byte image is padded to eight and succeeds" {
    const result = try withImage(&[_]u8{ 1, 2, 3, 4, 5 }, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqualStrings("", result.err);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000000 0x04030201\n") != null);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000004 0xFFFFFF05\n") != null);
}

test "a bare hex base needs no prefix" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "2000000" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x02000000 0x20010000\n") != null);
}

test "a negative base keeps Python's sign-then-pad spelling" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "-1" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x-0000001 0x20010000\n") != null);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000003 0x0200000D\n") != null);
}

test "a base above 32 bits is not truncated" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0xffffffff" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0xFFFFFFFF 0x20010000\n") != null);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x100000003 0x0200000D\n") != null);
}

test "a unicode-digit base parses as CPython's int would" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "\u{3000}0x10" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000010 0x20010000\n") != null);
}

test "the reset handler reaching the PC has its Thumb bit cleared" {
    const image = [_]u8{ 0x00, 0x00, 0x01, 0x20, 0xFF, 0xFF, 0xFF, 0xFF };
    const result = try withImage(&image, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0xE000EDF8 0xFFFFFFFE\nw4 0xE000EDF4 0x0001000F\n") != null);
    // The image write itself keeps the unmasked word.
    try testing.expect(std.mem.indexOf(u8, result.out, "w4 0x00000004 0xFFFFFFFF\n") != null);
}

test "the script ends with g then q and nothing after" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0" });
    defer result.deinit();
    try testing.expect(std.mem.endsWith(u8, result.out, "\ng\nq\n"));
}

test "an image path may sit in a subdirectory" {
    var tmp = testing.tmpDir(.{});
    defer tmp.cleanup();
    try tmp.dir.makeDir("build");
    try tmp.dir.writeFile(.{ .sub_path = "build/firmware.bin", .data = &vector_image });
    const result = try runIn(tmp.dir, &.{ "build/firmware.bin", "0x02000000" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expectEqualStrings("", result.err);
}

test "a large image emits one line per word" {
    var image: [4096]u8 = undefined;
    for (&image, 0..) |*byte, index| byte.* = @intCast(index & 0xFF);
    const result = try withImage(&image, &.{ "firmware.bin", "0x02000000" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    var lines = std.mem.tokenizeScalar(u8, result.out, '\n');
    var writes: usize = 0;
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "w4 0x02")) writes += 1;
    }
    try testing.expectEqual(@as(usize, 1024), writes);
}

test "an empty device string still prints its line" {
    const result = try withImage(&vector_image, &.{ "firmware.bin", "0x0", "--device", "" });
    defer result.deinit();
    try testing.expectEqual(@as(u8, 0), result.status);
    try testing.expect(std.mem.startsWith(u8, result.out, "device \n"));
}
