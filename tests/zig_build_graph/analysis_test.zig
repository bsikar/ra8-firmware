//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The analysis database's own rules: what makes two compile commands the
//! same command, which of them this graph can run itself, and the install path
//! it shares with the lint gate that parses it (#1157).
//!
//! In this file rather than in analysis.zig because `zig build test-zig` runs
//! ONE test root, tests/zig_build_graph/build_graph_test.zig, and reaches the
//! rest through that file's `_ = @import(...)`. A test declared in the build
//! module itself is compiled and never run, which is the quietest way to ship
//! an untested rule. Split from build_graph_test.zig for the 1000-line ceiling
//! scripts/checks/check_file_size.py holds every Zig source to, exactly as
//! cpu1_image_test.zig was.

const std = @import("std");
const graph = @import("build_graph");
const analysis = graph.analysis;
const compile_db = graph.compile_db;

test "a command class ignores the file and the object, and nothing else" {
    const base = compile_db.Entry{
        .file = "libs/ra8_core/src/ra8_log.c",
        .driver = "/opt/arm/bin/arm-none-eabi-gcc",
        .flags = &.{ "-mcpu=cortex-m85", "-std=gnu2x" },
        .include_dirs = &.{"libs/ra8_core/inc"},
        .object = "arm/blink_hal/ra8_log.c.o",
    };
    var other_file = base;
    other_file.file = "libs/ra8_core/src/ra8_scb.c";
    other_file.object = "arm/blink_hal/ra8_scb.c.o";
    var other_flags = base;
    other_flags.flags = &.{ "-mcpu=cortex-m33", "-std=gnu2x" };
    var other_includes = base;
    other_includes.include_dirs = &.{ "libs/ra8_core/inc", "libs/ra8_hal/inc" };

    const allocator = std.testing.allocator;
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();
    const scratch = arena.allocator();

    try std.testing.expectEqualStrings(
        analysis.commandClass(scratch, base),
        analysis.commandClass(scratch, other_file),
    );
    try std.testing.expect(!std.mem.eql(
        u8,
        analysis.commandClass(scratch, base),
        analysis.commandClass(scratch, other_flags),
    ));
    try std.testing.expect(!std.mem.eql(
        u8,
        analysis.commandClass(scratch, base),
        analysis.commandClass(scratch, other_includes),
    ));

    const entries = [_]compile_db.Entry{ base, other_file, other_flags, other_includes };
    const chosen = analysis.representatives(scratch, &entries);
    try std.testing.expectEqual(@as(usize, 3), chosen.len);
    try std.testing.expectEqualStrings(base.file, chosen[0].file);
    try std.testing.expectEqualStrings(other_flags.file, chosen[1].file);
}

test "only an absolutely-resolved driver is run by this step" {
    try std.testing.expect(analysis.isRunnableDriver("/opt/arm/bin/arm-none-eabi-gcc"));
    try std.testing.expect(!analysis.isRunnableDriver("clang"));
    try std.testing.expect(!analysis.isRunnableDriver("arm-none-eabi-gcc"));
}

test "the install path is the one the lint gate probes" {
    try std.testing.expectEqualStrings("zig-out/analysis/compile_commands.json", analysis.install_path);
}
