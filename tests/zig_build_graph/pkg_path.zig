// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Paths that may name a file inside a pinned build.zig.zon package.
//!
//! `pkg:<name>/<rel>` is `<rel>` inside package `<name>`, which zig fetches
//! once into its global cache. Any other path is repo-relative, as before.
//! The packages are lazy: the configure pass that first asks for one gets
//! nothing back, zig fetches it, and the configure runs again with it there.

const std = @import("std");
const src_tree = @import("src_tree.zig");

pub const prefix = "pkg:";

pub const Split = struct {
    name: []const u8,
    rel: []const u8,
};

/// The package name and the path inside it, or null for a repo path.
pub fn split(path: []const u8) ?Split {
    if (!std.mem.startsWith(u8, path, prefix)) return null;
    const rest = path[prefix.len..];
    const slash = std.mem.indexOfScalar(u8, rest, '/') orelse return .{ .name = rest, .rel = "." };
    return .{ .name = rest[0..slash], .rel = rest[slash + 1 ..] };
}

const Owner = struct { *std.Build, []const u8 };

/// The builder whose root `path` is relative to, and the path under it. Null
/// on the pass that asks zig to fetch the package.
fn owner(b: *std.Build, path: []const u8) ?Owner {
    const pkg = split(path) orelse return .{ b, path };
    const dep = b.lazyDependency(pkg.name, .{}) orelse return null;
    return .{ dep.builder, pkg.rel };
}

/// The directory to glob, or null while its package is not fetched yet, in
/// which case the caller contributes nothing to a graph zig throws away.
pub fn openDir(b: *std.Build, path: []const u8) ?std.Io.Dir {
    const root, const rel = owner(b, path) orelse return null;
    return src_tree.openDir(root, rel) catch |err| {
        std.debug.panic("ra8: cannot read directory '{s}': {s}", .{ path, @errorName(err) });
    };
}

/// The build input for `path`. On the fetch pass the placeholder is never
/// built, because zig reruns the configure before any step runs.
pub fn lazy(b: *std.Build, path: []const u8) std.Build.LazyPath {
    const pkg = split(path) orelse return b.path(path);
    const dep = b.lazyDependency(pkg.name, .{}) orelse return b.path("build.zig.zon");
    return dep.path(pkg.rel);
}

/// The absolute path, as the compile database writes it.
pub fn absolute(b: *std.Build, path: []const u8) []const u8 {
    const root, const rel = owner(b, path) orelse return path;
    return src_tree.absolute(root, rel);
}
