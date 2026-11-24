// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
//! Tests for app_local.zig source-dir resolution (RA8FW-918), proving pkg:
//! vendored source_dirs contribute sorted C translation units rather than
//! yielding empty archives.

const std = @import("std");
const graph = @import("build_graph");

const local = graph.app_local;
const pkg_path = graph.pkg_path;

const TmpOpenerCtx = struct {
    root: std.Io.Dir,
    pending_pkg: bool = false,

    fn open(raw_ctx: *anyopaque, dir_path: []const u8) ?std.Io.Dir {
        const self: *TmpOpenerCtx = @ptrCast(@alignCast(raw_ctx));
        if (pkg_path.split(dir_path)) |pkg| {
            if (self.pending_pkg) return null;
            const rel = std.mem.concat(std.testing.allocator, u8, &.{ pkg.name, "/", pkg.rel }) catch @panic("OOM");
            defer std.testing.allocator.free(rel);
            return self.root.openDir(std.testing.io, rel, .{ .iterate = true }) catch |err| {
                std.debug.panic("unexpected error opening {s}: {s}", .{ dir_path, @errorName(err) });
            };
        }
        return self.root.openDir(std.testing.io, dir_path, .{ .iterate = true }) catch |err| {
            std.debug.panic("unexpected error opening {s}: {s}", .{ dir_path, @errorName(err) });
        };
    }
};

fn freeCollected(allocator: std.mem.Allocator, items: []const []const u8) void {
    for (items) |item| allocator.free(item);
    allocator.free(items);
}

test "pkg: vendored source_dirs contribute sorted C units alongside repo dirs (RA8FW-918)" {
    var tmp = std.testing.tmpDir(.{});
    defer tmp.cleanup();

    try tmp.dir.createDirPath(std.testing.io, "tf_psa_crypto/core/nested");
    try tmp.dir.createDirPath(std.testing.io, "repo_vendor");

    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "tf_psa_crypto/core/z_last.c", .data = "" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "tf_psa_crypto/core/a_first.c", .data = "" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "tf_psa_crypto/core/ignored.h", .data = "" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "tf_psa_crypto/core/nested/sub.c", .data = "" });
    try tmp.dir.writeFile(std.testing.io, .{ .sub_path = "repo_vendor/local.c", .data = "" });

    const lib = local.VendoredLibrary{
        .name = "tfpsa_sb",
        .source_dirs = &.{ "pkg:tf_psa_crypto/core", "repo_vendor" },
        .system_include_dirs = &.{},
        .defines = &.{},
        .compile_options = &.{},
    };

    var ctx = TmpOpenerCtx{ .root = tmp.dir };
    const collected = local.collectSources(std.testing.allocator, std.testing.io, lib, &ctx, TmpOpenerCtx.open);
    defer freeCollected(std.testing.allocator, collected);

    try std.testing.expectEqual(@as(usize, 3), collected.len);
    try std.testing.expectEqualStrings("pkg:tf_psa_crypto/core/a_first.c", collected[0]);
    try std.testing.expectEqualStrings("pkg:tf_psa_crypto/core/z_last.c", collected[1]);
    try std.testing.expectEqualStrings("repo_vendor/local.c", collected[2]);

    // When lazyDependency returns null on the fetch pass, pkg: dirs defer cleanly.
    ctx.pending_pkg = true;
    const deferred = local.collectSources(std.testing.allocator, std.testing.io, lib, &ctx, TmpOpenerCtx.open);
    defer freeCollected(std.testing.allocator, deferred);

    try std.testing.expectEqual(@as(usize, 1), deferred.len);
    try std.testing.expectEqualStrings("repo_vendor/local.c", deferred[0]);
}

test "secure_boot_hil tfpsa_sb declares pkg: source_dirs parsed by pkg_path" {
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "secure_boot_hil")) continue;
        const lib = app.local.vendored.?;
        try std.testing.expect(lib.source_dirs.len > 0);
        for (lib.source_dirs) |dir_path| {
            const parsed = pkg_path.split(dir_path).?;
            try std.testing.expectEqualStrings("tf_psa_crypto", parsed.name);
        }
        return;
    }
    return error.TestExpectedSecureBootHil;
}
