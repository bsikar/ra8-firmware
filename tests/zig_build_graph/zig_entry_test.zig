//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The Zig-entry rule (RA8FW-503): an app that sets `zig_entry` links its
//! `src/main.zig` object where main.c would sit and compiles no main.c, and
//! every other app keeps main.c first. Reached from build_graph_test.zig's
//! `_ = @import(...)`, so it runs under `zig build test-zig`.

const std = @import("std");
const graph = @import("build_graph");
const sources = graph.cross_sources;
const zig_entry = graph.zig_entry;

fn appNamed(name: []const u8) sources.CrossApp {
    for (graph.cross_apps) |app| {
        if (std.mem.eql(u8, app.name, name)) return app;
    }
    @panic("app not in the cross table");
}

test "exactly one app in the table takes a Zig entry" {
    var with_entry: usize = 0;
    for (graph.cross_apps) |app| {
        if (app.zig_entry) with_entry += 1;
    }
    try std.testing.expectEqual(@as(usize, 1), with_entry);
    try std.testing.expect(appNamed("threadx_stkof").zig_entry);
}

test "the Zig-entry app keeps a main.zig and no main.c on disk" {
    const app = appNamed("threadx_stkof");
    var dir = try std.fs.cwd().openDir(app.dir, .{});
    defer dir.close();
    try dir.access(zig_entry.root_name, .{});
    try std.testing.expectError(error.FileNotFound, dir.access("src/main.c", .{}));
}

test "the entry path is spelled from the repository root" {
    const app = appNamed("threadx_stkof");
    const expected = "examples/ek_ra8d2/hil_needs_revalidation/threadx_stkof/src/main.zig";
    var buffer: [256]u8 = undefined;
    const spelled = try std.fmt.bufPrint(&buffer, "{s}/{s}", .{ app.dir, zig_entry.root_name });
    try std.testing.expectEqualStrings(expected, spelled);
}

test "every other app still names a main.c" {
    for (graph.cross_apps) |app| {
        if (app.zig_entry) continue;
        var buffer: [256]u8 = undefined;
        const main_c = try std.fmt.bufPrint(&buffer, "{s}/src/main.c", .{app.dir});
        try std.fs.cwd().access(main_c, .{});
    }
}
