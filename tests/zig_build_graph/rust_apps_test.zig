//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the paired Rust cross-build app registry (`rust_apps.zig`).

const std = @import("std");
const graph = @import("build_graph");
const rust_apps = graph.rust_apps;
const sources = graph.cross_sources;

fn findCrossAppByName(name: []const u8) ?sources.CrossApp {
    for (graph.cross_apps) |app| {
        if (std.mem.eql(u8, app.name, name)) return app;
    }
    return null;
}

test "ra8_box_pair_rs matches ra8_box_pair_c board, linker script, stack, and Zig library" {
    try std.testing.expectEqual(@as(usize, 1), rust_apps.rust_apps.len);
    const rs = rust_apps.rust_apps[0];
    const c_peer = findCrossAppByName("ra8_box_pair_c") orelse return error.MissingPeerApp;

    try std.testing.expectEqualStrings("ra8_box_pair_rs", rs.cross.name);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/ra8_box_pair_rs",
        rs.cross.dir,
    );
    try std.testing.expectEqualStrings("thumbv8m.main-none-eabihf", rs.rust_target);
    try std.testing.expectEqualStrings("libra8_box_pair_rs.a", rs.archive_name);

    try std.testing.expectEqualStrings(c_peer.board, rs.cross.board);
    try std.testing.expectEqualStrings(c_peer.linker_script, rs.cross.linker_script);
    try std.testing.expectEqual(c_peer.stack_bytes, rs.cross.stack_bytes);
    try std.testing.expectEqualSlices([]const u8, c_peer.libraries, rs.cross.libraries);
    try std.testing.expectEqualSlices([]const u8, c_peer.zig_libraries, rs.cross.zig_libraries);
}

test "archive-backed Rust apps suppress src/main.c while retaining board boot resolution" {
    const rs = rust_apps.rust_apps[0].cross;
    const c_peer = findCrossAppByName("ra8_box_pair_c") orelse return error.MissingPeerApp;

    try std.testing.expect(sources.includesCMain(c_peer, true));
    try std.testing.expect(!sources.includesCMain(rs, false));

    const vector_table = sources.bootSourcePath(std.testing.allocator, rs, "vector_table.c", false);
    defer std.testing.allocator.free(vector_table);
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/src/boot/vector_table.c", vector_table);
}
