//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const app = @import("image_pyramid");

const FailingWriter = struct {
    const Error = error{InjectedFailure};
    completed_lines: usize = 0,

    fn write(self: *FailingWriter, bytes: []const u8) Error!usize {
        if (self.completed_lines >= 1) return error.InjectedFailure;
        if (std.mem.indexOfScalar(u8, bytes, '\n')) |newline| {
            self.completed_lines += 1;
            return newline + 1;
        }
        return bytes.len;
    }

    fn writer(self: *FailingWriter) std.io.Writer(*FailingWriter, Error, write) {
        return .{ .context = self };
    }
};

test "help is stable" {
    var output = std.ArrayList(u8).init(std.testing.allocator);
    defer output.deinit();
    var errors = std.ArrayList(u8).init(std.testing.allocator);
    defer errors.deinit();
    const args = [_][]const u8{ "image_pyramid", "--help" };
    const status = try app.execute(std.testing.allocator, &args, output.writer(), errors.writer());
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqualStrings("usage: image_pyramid <input.jpg> --out-dir <dir> [--levels <1..16>]\n", output.items);
    try std.testing.expectEqual(@as(usize, 0), errors.items.len);
}

test "missing output directory is a usage error" {
    var output = std.ArrayList(u8).init(std.testing.allocator);
    defer output.deinit();
    var errors = std.ArrayList(u8).init(std.testing.allocator);
    defer errors.deinit();
    const args = [_][]const u8{ "image_pyramid", "dog.jpg" };
    const status = try app.execute(std.testing.allocator, &args, output.writer(), errors.writer());
    try std.testing.expectEqual(@as(u8, 2), status);
    try std.testing.expect(std.mem.startsWith(u8, errors.items, "error: missing-output-dir\n"));
}

test "dog fixture produces the intentionally degraded eight-level pyramid" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realpathAlloc(allocator, ".");
    defer allocator.free(output_path);

    var output = std.ArrayList(u8).init(allocator);
    defer output.deinit();
    var errors = std.ArrayList(u8).init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
    };
    const status = try app.execute(allocator, &args, output.writer(), errors.writer());
    try std.testing.expectEqual(@as(u8, 0), status);
    try std.testing.expectEqual(@as(usize, 0), errors.items.len);

    const names = [_][]const u8{
        "level-01-165x124-q25.jpg",
        "level-02-83x62-q25.jpg",
        "level-03-42x31-q25.jpg",
        "level-04-21x16-q25.jpg",
        "level-05-11x8-q25.jpg",
        "level-06-6x4-q25.jpg",
        "level-07-3x2-q25.jpg",
        "level-08-2x1-q25.jpg",
    };
    const hashes = [_][]const u8{
        "295aedbcff8f0d9c3658649f9ea61ad3a88dd28ac166b6c3439b981b9de12f2e",
        "7a1fcc27c33867576dc6b721423a91c861adbb78801d63561dd3c72741e7e7af",
        "1603c9aef2dfaeb78db687c149b2b145db8ece14751bf9f7e5e5753d8949b104",
        "0c35f37e8e49f82e67b035803e807e4cdf13b85755f246cdd1562a3833786712",
        "84b41cb4f9bcfee51e4cc466a1f95cbb4398089f42fea0c283366c6320c67dc8",
        "231817e28969ec95364b2e740edec59b986782e894df21a89d3afda3188cc196",
        "c796092a2601ad9594715242a02240ec7e9ddc698d12c163ee6c36736a614d9e",
        "79cd6a0a0fdaf9f46a8a5af9cbfffa26589eae299fcccded6c9a921e4dab9a3d",
    };
    for (names, hashes) |name, expected_hash| {
        const bytes = try temporary.dir.readFileAlloc(allocator, name, 16 * 1024 * 1024);
        defer allocator.free(bytes);
        try std.testing.expect(bytes.len > 4);
        try std.testing.expectEqualSlices(u8, &.{ 0xff, 0xd8 }, bytes[0..2]);
        try std.testing.expectEqualSlices(u8, &.{ 0xff, 0xd9 }, bytes[bytes.len - 2 ..]);
        var digest: [32]u8 = undefined;
        std.crypto.hash.sha2.Sha256.hash(bytes, &digest, .{});
        const actual_hash = std.fmt.bytesToHex(digest, .lower);
        try std.testing.expectEqualStrings(expected_hash, &actual_hash);
    }
    try std.testing.expectEqual(@as(usize, 8), std.mem.count(u8, output.items, "level="));
}

test "manifest failure rolls back every published output" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realpathAlloc(allocator, ".");
    defer allocator.free(output_path);

    var failing = FailingWriter{};
    var errors = std.ArrayList(u8).init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
        "--levels",
        "2",
    };
    try std.testing.expectError(error.InjectedFailure, app.execute(allocator, &args, failing.writer(), errors.writer()));
    try std.testing.expectEqual(@as(usize, 1), failing.completed_lines);

    var iterable = try temporary.dir.openDir(".", .{ .iterate = true });
    defer iterable.close();
    var entries = iterable.iterate();
    try std.testing.expect((try entries.next()) == null);
}

test "existing output is preserved and prevents publication" {
    const allocator = std.testing.allocator;
    var temporary = std.testing.tmpDir(.{});
    defer temporary.cleanup();
    const output_path = try temporary.dir.realpathAlloc(allocator, ".");
    defer allocator.free(output_path);
    try temporary.dir.writeFile(.{ .sub_path = "level-01-165x124-q25.jpg", .data = "keep-me" });

    var output = std.ArrayList(u8).init(allocator);
    defer output.deinit();
    var errors = std.ArrayList(u8).init(allocator);
    defer errors.deinit();
    const args = [_][]const u8{
        "image_pyramid",
        "fixtures/dog-source.jpg",
        "--out-dir",
        output_path,
        "--levels",
        "1",
    };
    const status = try app.execute(allocator, &args, output.writer(), errors.writer());
    try std.testing.expectEqual(@as(u8, 1), status);
    try std.testing.expect(std.mem.startsWith(u8, errors.items, "error: output-collision:"));
    const existing = try temporary.dir.readFileAlloc(allocator, "level-01-165x124-q25.jpg", 64);
    defer allocator.free(existing);
    try std.testing.expectEqualStrings("keep-me", existing);
}
