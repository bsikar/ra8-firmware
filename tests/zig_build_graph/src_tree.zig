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
///
/// Zig opens every recorded dependency when it checks the cached
/// configuration, so a missing path can't be recorded as itself. Its nearest
/// existing ancestor's listing is recorded instead: that is what changes when
/// the path appears.
pub fn exists(b: *std.Build, rel: []const u8) bool {
    const stat = b.root.statFile(b.graph.io, rel) catch {
        watchCreation(b, rel);
        return false;
    };
    if (stat.kind == .directory) {
        b.dependOnDirectoryMetadata(b.path(rel));
    } else {
        b.dependOnFileMetadata(b.path(rel));
    }
    return true;
}

/// Record the listing of the nearest existing directory above `rel`.
fn watchCreation(b: *std.Build, rel: []const u8) void {
    var parent = std.fs.path.dirname(rel);
    while (parent) |dir| : (parent = std.fs.path.dirname(dir)) {
        const stat = b.root.statFile(b.graph.io, dir) catch continue;
        if (stat.kind != .directory) break;
        b.dependOnDirectoryContents(b.path(dir));
        return;
    }
    b.dependOnDirectoryContents(b.path("."));
}

/// `rel` opened for one non-recursive listing.
/// A directory that can't be opened is recorded the way `exists` records a
/// missing path.
pub fn openDir(b: *std.Build, rel: []const u8) std.Io.Dir.OpenError!std.Io.Dir {
    const dir = b.root.openDir(b.graph.io, rel, .{ .iterate = true }) catch |err| {
        watchCreation(b, rel);
        return err;
    };
    b.dependOnDirectoryContents(b.path(rel));
    return dir;
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

/// Whether `path`, outside the source tree (a toolchain binary), exists.
/// Recorded like `exists`, against the working directory.
pub fn existsOutside(b: *std.Build, path: []const u8) bool {
    const io = b.graph.io;
    if (std.Io.Dir.cwd().statFile(io, path, .{})) |stat| {
        if (stat.kind == .directory) {
            b.dependOnDirectoryMetadata(b.graph.cwdRelativePath(path));
        } else {
            b.dependOnFileMetadata(b.graph.cwdRelativePath(path));
        }
        return true;
    } else |_| {}
    var parent = std.fs.path.dirname(path);
    while (parent) |dir| : (parent = std.fs.path.dirname(dir)) {
        const stat = std.Io.Dir.cwd().statFile(io, dir, .{}) catch continue;
        if (stat.kind != .directory) break;
        b.dependOnDirectoryContents(b.graph.cwdRelativePath(dir));
        break;
    }
    return false;
}
