//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! An M85 image whose main is a Zig root (RA8FW-408).

const std = @import("std");
const graph = @import("build_graph");
const sources = graph.cross_sources;

const zig_app = sources.CrossApp{
    .name = "probe",
    .dir = "examples/ek_ra8d2/probe",
    .board = "libs/ra8_board_ek_ra8d2",
    .linker_script = "libs/ra8_board_ek_ra8d2/ld/linker_script.ld",
    .libraries = &.{},
    .zig_libraries = &.{},
    .zig_main = "src/main.zig",
};

test "an app with a Zig main has no main.c in its C set" {
    try std.testing.expect(!sources.hasCMain(zig_app));
}

test "threadx_cpu1, npu_vela_conv, txm_manager_cpu1, txm_fault_cpu1 and cpu1_pingpong_ra8p1 are the apps in the table with a Zig main" {
    const expected = [_][]const u8{ "threadx_cpu1", "npu_vela_conv", "txm_manager_cpu1", "txm_fault_cpu1", "cpu1_pingpong_ra8p1" };
    var zig_mains: usize = 0;
    for (sources.cross_apps) |app| {
        if (sources.hasCMain(app)) continue;
        try std.testing.expect(zig_mains < expected.len);
        try std.testing.expectEqualStrings(expected[zig_mains], app.name);
        try std.testing.expectEqualStrings("src/main.zig", app.zig_main.?);
        zig_mains += 1;
    }
    try std.testing.expectEqual(expected.len, zig_mains);
}

test "an app's Zig code is built for the M85 with the hard float ABI" {
    const q = sources.zig_target_query;
    try std.testing.expectEqual(std.Target.Cpu.Arch.thumb, q.cpu_arch.?);
    try std.testing.expectEqual(std.Target.Os.Tag.freestanding, q.os_tag.?);
    try std.testing.expectEqual(std.Target.Abi.eabihf, q.abi.?);
    try std.testing.expectEqualStrings("cortex_m85", q.cpu_model.explicit.name);
}

test "a Zig main imports nothing unless its row names a module" {
    try std.testing.expectEqual(@as(usize, 0), zig_app.zig_main_imports.len);
    for (sources.cross_apps) |app| {
        if (sources.hasCMain(app)) try std.testing.expectEqual(@as(usize, 0), app.zig_main_imports.len);
    }
}

test "a named import carries its module name and a repo-relative root" {
    const row = sources.CrossApp{
        .name = "probe",
        .dir = "examples/ra8p1_foundation/probe",
        .board = "libs/ra8_board_ra8p1",
        .linker_script = "libs/ra8_board_ra8p1/ld/linker_script.ld",
        .libraries = &.{},
        .zig_libraries = &.{},
        .zig_main = "src/main.zig",
        .zig_main_imports = &.{.{ .name = "golden", .path = "tools/vela/generated/conv_int8_vela_golden.zig" }},
    };
    const import = row.zig_main_imports[0];
    try std.testing.expectEqualStrings("golden", import.name);
    try std.testing.expect(!std.mem.startsWith(u8, import.path, row.dir));
    try std.testing.expect(std.mem.endsWith(u8, import.path, ".zig"));
}
