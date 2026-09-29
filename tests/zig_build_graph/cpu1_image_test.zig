//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The second (Cortex-M33 / CPU1) image's own rules: which translation units
//! it compiles, the flag order that decides which core it is built for, and
//! the include path it is allowed to reach.
//!
//! Split out of build_graph_test.zig, which reached the 1000-line ceiling
//! scripts/checks/check_file_size.py holds every Zig source to. Same module,
//! reached from that file's `_ = @import(...)`, so these run under
//! `zig build test-zig` exactly as they did inline.
//!
//! Two dual-core apps are in the table and they disagree about one directory
//! on this path, which is the whole reason it is per-app data rather than a
//! constant here: cpu1_pingpong keeps the board layer, cpu1_pingpong_ipc
//! stops before it (#1146).

const std = @import("std");
const graph = @import("build_graph");
const cpu1 = graph.cpu1_image;
const sources = graph.cross_sources;
const arm_flags = graph.arm_flags;

/// The two dual-core apps: the one whose CPU1 include path carries the board
/// layer, and the one whose does not. `bare_app` is the single-image control,
/// here so "this app has no second image at all" stays asserted beside the
/// apps that do.
const bare_app = graph.cross_apps[0];
const library_app = graph.cross_apps[1];
const dual_core_app = graph.cross_apps[2];
const no_nsc_app = graph.cross_apps[9];

fn cpu1App(app: @TypeOf(dual_core_app)) cpu1.App {
    return .{ .name = app.name, .dir = app.dir, .board = app.board };
}

fn indexOf(haystack: []const []const u8, needle: []const u8) ?usize {
    for (haystack, 0..) |item, i| {
        if (std.mem.eql(u8, item, needle)) return i;
    }
    return null;
}

test "the second image compiles exactly the four units its executable names" {
    const allocator = std.testing.allocator;
    const image = dual_core_app.cpu1 orelse return error.MissingCpu1Image;
    const app = cpu1App(dual_core_app);

    const units = cpu1.sources(allocator, app, image);
    defer allocator.free(units);
    defer allocator.free(units[0]);
    try std.testing.expectEqual(@as(usize, 4), units.len);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/src/cpu1_main.c",
        units[0],
    );
    try std.testing.expectEqualStrings("libs/ra8_hal/src/ra8_ipc.c", units[1]);

    // The entry unit is the same file AUX_SRCS keeps out of the M85 image: one
    // file, two images, and each rule is the other's mirror.
    try std.testing.expect(sources.isAuxSource(dual_core_app, image.entry_source));
    try std.testing.expect(!sources.appLocalIsCompiled(dual_core_app, image.entry_source));

    // A single-core app has no second image at all.
    try std.testing.expect(bare_app.cpu1 == null);
    try std.testing.expect(library_app.cpu1 == null);
}

test "the second image's flags override the inherited ones, in that order" {
    const allocator = std.testing.allocator;
    const global = [_][]const u8{ "-mcpu=cortex-m85", "-fdata-sections", "-O0", "-std=gnu2x" };
    const flags = cpu1.compileFlags(allocator, &global);
    defer allocator.free(flags);

    // gcc takes the last -mcpu and the last -O, so the inherited M85 flags
    // must come FIRST and the M33 target's own after them. Reversed, this
    // builds the second core's image for the first core's core.
    try std.testing.expect(indexOf(flags, "-mcpu=cortex-m85").? < indexOf(flags, "-mcpu=cortex-m33").?);
    try std.testing.expect(indexOf(flags, "-O0").? < indexOf(flags, "-Os").?);

    // And the inherited set survives: dropping it would change the image.
    try std.testing.expect(indexOf(flags, "-fdata-sections") != null);
    try std.testing.expect(indexOf(flags, "-std=gnu2x") != null);
    try std.testing.expect(indexOf(flags, "-DRA8_BUILD_FOR_CPU1") != null);

    // No first-party warning profile: those ride on ra8_add_app() targets, and
    // this executable is hand-rolled in the app's own CMakeLists.
    try std.testing.expect(indexOf(flags, "-Werror") == null);
    try std.testing.expect(indexOf(flags, "-Wstack-usage=2200") == null);
}

test "the second image's include path is the narrow one, not the app's" {
    const allocator = std.testing.allocator;
    const app = cpu1App(dual_core_app);
    const image = dual_core_app.cpu1 orelse return error.MissingCpu1Image;
    const dirs = cpu1.includeDirs(allocator, app, image);
    defer allocator.free(dirs);
    defer for ([_]usize{ 0, 1, 4 }) |owned| allocator.free(dirs[owned]);

    try std.testing.expectEqual(@as(usize, 5), dirs.len);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hw_validated/hil/cpu1_pingpong/inc",
        dirs[0],
    );
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/inc", dirs[4]);

    // Only freestanding-clean headers may be reached from a Cortex-M33 TU, so
    // the four library directories the M85 image carries are absent here.
    for ([_][]const u8{
        "libs/ra8_net_pal/inc",
        "libs/ra8_usb_pal/inc",
        "libs/ra8_nsc/inc",
        "libs/ra8_secure_app/inc",
    }) |absent| {
        try std.testing.expect(indexOf(dirs, absent) == null);
    }
}

test "the second image's board include directory is per-app, and both apps are in the table" {
    const allocator = std.testing.allocator;
    const image = no_nsc_app.cpu1 orelse return error.MissingCpu1Image;
    try std.testing.expect(!image.board_include_dir);

    const dirs = cpu1.includeDirs(allocator, cpu1App(no_nsc_app), image);
    defer allocator.free(dirs);
    defer for ([_]usize{ 0, 1 }) |owned| allocator.free(dirs[owned]);

    // Four directories, stopping before the board layer, where the other
    // dual-core app carries five. Nothing on disk says which; it is the one
    // difference between two hand-rolled CPU1 targets.
    try std.testing.expectEqual(@as(usize, 4), dirs.len);
    try std.testing.expectEqualStrings(
        "examples/ek_ra8d2/hil_needs_revalidation/cpu1_pingpong_ipc/inc",
        dirs[0],
    );
    try std.testing.expectEqualStrings("libs/ra8_hal/inc", dirs[3]);
    try std.testing.expect(indexOf(dirs, "libs/ra8_board_ek_ra8d2/inc") == null);

    // The other arm, on the app that keeps it.
    const other = dual_core_app.cpu1 orelse return error.MissingCpu1Image;
    try std.testing.expect(other.board_include_dir);
}

test "a TrustZone app's second image carries neither -mcmse nor the TrustZone define" {
    const allocator = std.testing.allocator;
    try std.testing.expect(no_nsc_app.trust_zone);

    const global = [_][]const u8{ "-mcpu=cortex-m85", "-O0", "-g3", "-DDEBUG", "-std=gnu2x" };
    const flags = cpu1.compileFlags(allocator, &global);
    defer allocator.free(flags);

    // Both ride on the ra8_add_app() target, and the CPU1 executable is
    // declared by hand, so neither reaches the M33 units even though every
    // unit of the M85 image beside them carries both.
    try std.testing.expect(indexOf(flags, arm_flags.trust_zone.cmse) == null);
    try std.testing.expect(indexOf(flags, arm_flags.trust_zone.define) == null);
    // RA8_FREESTANDING does reach it: the toolchain file adds that one at
    // DIRECTORY scope, so it is not a target property to lose.
    try std.testing.expect(indexOf(flags, "-DRA8_FREESTANDING") != null);
    try std.testing.expect(indexOf(flags, "-DRA8_BUILD_FOR_CPU1") != null);
}
