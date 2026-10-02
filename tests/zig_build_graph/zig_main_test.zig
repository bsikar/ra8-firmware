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

test "threadx_cpu1 is the one app in the table with a Zig main" {
    var zig_mains: usize = 0;
    for (sources.cross_apps) |app| {
        if (sources.hasCMain(app)) continue;
        zig_mains += 1;
        try std.testing.expectEqualStrings("threadx_cpu1", app.name);
        try std.testing.expectEqualStrings("src/main.zig", app.zig_main.?);
    }
    try std.testing.expectEqual(@as(usize, 1), zig_mains);
}

test "an app's Zig code is built for the M85 with the hard float ABI" {
    const q = sources.zig_target_query;
    try std.testing.expectEqual(std.Target.Cpu.Arch.thumb, q.cpu_arch.?);
    try std.testing.expectEqual(std.Target.Os.Tag.freestanding, q.os_tag.?);
    try std.testing.expectEqual(std.Target.Abi.eabihf, q.abi.?);
    try std.testing.expectEqualStrings("cortex_m85", q.cpu_model.explicit.name);
}
