// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Configure-time reads of a builder's source tree.
//!
//! Zig caches the configuration and skips the configure phase when nothing
//! it tracked has changed, so every read here registers what it observed.
//! A read that skipped that would leave a stale graph after a source file is
//! added or removed.

const std = @import("std");

/// Whether `rel` exists, as a file or a directory.
pub fn exists(b: *std.Build, rel: []const u8) bool {
    b.dependOnFileMetadata(b.path(rel));
    b.root.access(b.graph.io, rel, .{}) catch return false;
    return true;
}

/// `rel` opened for one non-recursive listing.
pub fn openDir(b: *std.Build, rel: []const u8) std.Io.Dir.OpenError!std.Io.Dir {
    b.dependOnDirectoryContents(b.path(rel));
    return b.root.openDir(b.graph.io, rel, .{ .iterate = true });
}

/// `rel` opened for a recursive walk. Nested entries can't be registered one
/// by one, so the configuration is rebuilt on every run instead.
pub fn openTree(b: *std.Build, rel: []const u8) std.Io.Dir.OpenError!std.Io.Dir {
    b.graph.poisonCache();
    return b.root.openDir(b.graph.io, rel, .{ .iterate = true });
}

/// The contents of file `rel`, at most `limit` bytes.
pub fn readFile(b: *std.Build, rel: []const u8, limit: usize) ![]u8 {
    b.dependOnFileContents(b.path(rel));
    const full = try b.root.joinString(b.allocator, rel);
    return std.Io.Dir.cwd().readFileAlloc(b.graph.io, full, b.allocator, .limited(limit));
}

/// The absolute path of `rel`.
pub fn absolute(b: *std.Build, rel: []const u8) []const u8 {
    const cwd = std.process.currentPathAlloc(b.graph.io, b.allocator) catch |err|
        std.debug.panic("ra8: cannot read the working directory: {s}", .{@errorName(err)});
    const full = b.root.joinString(b.allocator, rel) catch @panic("OOM");
    return b.pathResolve(&.{ cwd, full });
}
