// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
//! The Zig entry object of a cross-built app (RA8FW-503).
//!
//! An app that sets `zig_entry` keeps `src/main.zig` where every other app
//! keeps `src/main.c`. The root is compiled once for the image's cortex_m85
//! target at the configuration's optimisation and linked as the first object,
//! the slot main.c takes in ra8_add_app()'s order. Unwind tables are off for
//! the same reason the ra8_core image root turns them off: the board scripts
//! keep no .ARM.exidx output section for them to land in.

const std = @import("std");
const app_table = @import("app_table.zig");

/// Where an app keeps its Zig entry, relative to the app directory.
pub const root_name = "src/main.zig";

/// The entry's path, spelled from the repository root.
pub fn rootPath(b: *std.Build, app: app_table.CrossApp) []const u8 {
    return b.fmt("{s}/{s}", .{ app.dir, root_name });
}

/// Compile the app's Zig entry and return the emitted object.
pub fn object(
    b: *std.Build,
    app: app_table.CrossApp,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) std.Build.LazyPath {
    const root = b.createModule(.{
        .root_source_file = b.path(rootPath(b, app)),
        .target = target,
        .optimize = optimize,
        .unwind_tables = .none,
    });
    const entry = b.addObject(.{
        .name = b.fmt("{s}_main", .{app.name}),
        .root_module = root,
    });
    return entry.getEmittedBin();
}
