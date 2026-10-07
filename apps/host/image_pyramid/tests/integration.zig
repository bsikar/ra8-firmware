//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const app = @import("image_pyramid");

/// Accepts one complete line, then fails every later write.
const FailingWriter = struct {
    completed_lines: usize = 0,
    interface: std.Io.Writer = .{ .vtable = &.{ .drain = drain }, .buffer = &.{} },

    fn drain(writer: *std.Io.Writer, data: []const []const u8, splat: usize) std.Io.Writer.Error!usize {
        const self: *FailingWriter = @alignCast(@fieldParentPtr("interface", writer));
        if (self.completed_lines >= 1) return error.WriteFailed;
        const bytes = for (data[0 .. data.len - 1]) |slice| {
            if (slice.len != 0) break slice;
        } else if (splat == 0) return 0 else data[data.len - 1];
        if (std.mem.indexOfScalar(u8, bytes, '\n')) |newline| {
            self.completed_lines += 1;
            return newline + 1;
        }
        return bytes.len;
    }
};

test "help is stable" {
    var output: std.Io.Writer.Allocating = .init(std.testing.allocator);
    defer output.deinit();
    var errors: std.Io.Writer.Allocating = .init(std.testing.allocator);
    defer errors.deinit();
    const args = [_][]const u8{ "image_pyramid", "--help" };
    const status = try app.execute(std.testing.allocator, std.testing.io, &args, &output.writer, &errors.writer);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqualStrings("usage: image_pyramid <input.jpg> --out-dir <dir> [--levels <1..16>]\n", output.written());
    try std.testing.expectEqual(@as(usize, 0), errors.written().len);
}

test "missing output directory is a usage error" {
    var output: std.Io.Writer.Allocating = .init(std.testing.allocator);
    defer output.deinit();
    var errors: std.Io.Writer.Allocating = .init(std.testing.allocator);
    defer errors.deinit();
    const args = [_][]const u8{ "image_pyramid", "dog.jpg" };
    const status = try app.execute(std.testing.allocator, std.testing.io, &args, &output.writer, &errors.writer);
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expect(std.mem.startsWith(u8, errors.written(), "error: missing-output-dir\n"));
}

test "dog fixture produces the intentionally degraded eight-level pyramid" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realPathFileAlloc(std.testing.io, ".", allocator);
    defer allocator.free(output_path);

    var output: std.Io.Writer.Allocating = .init(allocator);
    defer output.deinit();
    var errors: std.Io.Writer.Allocating = .init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
    };
    const status = try app.execute(allocator, std.testing.io, &args, &output.writer, &errors.writer);
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqual(@as(usize, 0), errors.written().len);

    const names = [_][]const u8{
        "level-01-165x247-q25.jpg",
        "level-02-165x124-q25.jpg",
        "level-03-83x124-q25.jpg",
        "level-04-83x62-q25.jpg",
        "level-05-42x62-q25.jpg",
        "level-06-42x31-q25.jpg",
        "level-07-21x31-q25.jpg",
        "level-08-21x16-q25.jpg",
    };
    const hashes = [_][]const u8{
        "e58fd41bbb7f4201104c43a43f61ef78773130f6ef2e55a81348a9df7ab4c82c",
        "e2401732089f9d8bf54c3da06fe450d78f1896cbd7529713f87063818131a100",
        "1124de51590fd03f3679449a92b599165b361d119f6c0870e20cdd2824372236",
        "b38cef58fca6f93b2237e26889743cd7a1061c01a2376fe5247c4f9234c9db48",
        "fc285cd60fbbc8ef5e2c51604753568ed82829108b1b3bbe01ec23ec0ca0b789",
        "95e7a8fff6d0007c5409703ca2b150b9f3fc1ad41d6aba5028674dd5cbbbdb9e",
        "107cfc375c2190e2f802e9d264c184e00ac29c7ba2bcfb3d0ab05dad28bb8211",
        "eeabda13201198619c6bbc8e5582151db52117ad79083548e3904f8321591910",
    };
    for (names, hashes) |name, expected_hash| {
        const bytes = try temporary.dir.readFileAlloc(std.testing.io, name, allocator, .limited(16 * 1024 * 1024));
        defer allocator.free(bytes);
        try std.testing.expect(bytes.len > 4);
        try std.testing.expectEqualSlices(u8, &.{ 0xff, 0xd8 }, bytes[0..2]);
        try std.testing.expectEqualSlices(u8, &.{ 0xff, 0xd9 }, bytes[bytes.len - 2 ..]);
        var digest: [32]u8 = undefined;
        std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
        const actual_hash = std.fmt.bytesToHex(digest, .lower);
        try std.testing.expectEqualStrings(expected_hash, &actual_hash);
    }
    try std.testing.expectEqual(@as(usize, 8), std.mem.count(u8, output.written(), "level="));
}

test "manifest failure rolls back every published output" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realPathFileAlloc(std.testing.io, ".", allocator);
    defer allocator.free(output_path);

    var failing = FailingWriter{};
    var errors: std.Io.Writer.Allocating = .init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
        "--levels",
        "2",
    };
    try std.testing.expectError(error.WriteFailed, app.execute(allocator, std.testing.io, &args, &failing.interface, &errors.writer));
    try std.testing.expectEqual(@as(usize, 1), failing.completed_lines);

    var iterable = try temporary.dir.openDir(std.testing.io, ".", .{ .iterate = true });
    defer iterable.close(std.testing.io);
    var entries = iterable.iterate();
    try std.testing.expect((try entries.next(std.testing.io)) == null);
}

test "existing output is preserved and prevents publication" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realPathFileAlloc(std.testing.io, ".", allocator);
    defer allocator.free(output_path);
    try temporary.dir.writeFile(std.testing.io, .{ .sub_path = "level-01-165x247-q25.jpg", .data = "keep-me" });

    var output: std.Io.Writer.Allocating = .init(allocator);
    defer output.deinit();
    var errors: std.Io.Writer.Allocating = .init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
        "--levels",
        "1",
    };
    const status = try app.execute(allocator, std.testing.io, &args, &output.writer, &errors.writer);
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.startsWith(u8, errors.written(), "error: output-collision:"));
    const existing = try temporary.dir.readFileAlloc(std.testing.io, "level-01-165x247-q25.jpg", allocator, .limited(64));
    defer allocator.free(existing);
    try std.testing.expectEqualStrings("keep-me", existing);
}
