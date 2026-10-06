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
//! stops before it.

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

test "the second image compiles exactly the units its executable names" {
    const allocator = std.testing.allocator;
    const image = dual_core_app.cpu1 orelse return error.MissingCpu1Image;
    const app = cpu1App(dual_core_app);

    const units = cpu1.sources(allocator, app, image);
    defer allocator.free(units);
    defer allocator.free(units[0]);
    // Two, not three: libs/ra8_core/src/ra8_scb.c was the third until the
    // fault-block port to Zig deleted it. The SCB window is in
    // ra8_core's archive now, which this image links built for its own core.
    try std.testing.expectEqual(@as(usize, 2), units.len);
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

fn has(items: []const []const u8, needle: []const u8) bool {
    for (items) |item| {
        if (std.mem.indexOf(u8, item, needle) != null) return true;
    }
    return false;
}

const middleware_app = cpu1.App{ .name = "threadx_cpu1", .dir = "examples/ek_ra8d2/threadx_cpu1", .board = "libs/ra8_board_ek_ra8d2" };
const middleware_image = cpu1.Cpu1Image{
    .entry_source = "src/cpu1_main.c",
    .shared_sources = &.{},
    .linker_script = "linker_script_cpu1.ld",
    .uses = &.{"threadx_m33"},
};

test "a CPU1 image that uses threadx_m33 gets its defines and include paths" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    const flags = cpu1.unitFlags(a, middleware_image, &.{"-mcpu=cortex-m85"});
    try std.testing.expectEqualStrings("-DTX_INCLUDE_USER_DEFINE_FILE", flags[flags.len - 1]);
    try std.testing.expect(has(flags, "-mcpu=cortex-m33"));
    const dirs = cpu1.unitIncludeDirs(a, middleware_app, middleware_image);
    try std.testing.expectEqualStrings("port/threadx/inc", dirs[dirs.len - 1]);
    const system = cpu1.systemIncludeDirs(a, middleware_image);
    try std.testing.expect(has(system, "ports/cortex_m33/gnu/inc"));
    try std.testing.expect(!has(system, "cortex_m85"));
}

test "a CPU1 image with no middleware keeps exactly its old flags and path" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const a = arena.allocator();
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        if (image.uses.len != 0) continue;
        const view = cpu1.App{ .name = app.name, .dir = app.dir, .board = app.board };
        const global = &[_][]const u8{ "-mcpu=cortex-m85", "-O0" };
        try std.testing.expectEqualDeep(cpu1.compileFlags(a, global), cpu1.unitFlags(a, image, global));
        try std.testing.expectEqualDeep(cpu1.includeDirs(a, view, image), cpu1.unitIncludeDirs(a, view, image));
        try std.testing.expectEqual(@as(usize, 0), cpu1.systemIncludeDirs(a, image).len);
    }
}

const zig_entry_image = cpu1.Cpu1Image{
    .entry_source = "src/cpu1_main.zig",
    .shared_sources = &.{"libs/ra8_hal/src/ra8_ipc.c"},
    .linker_script = "linker_script_cpu1.ld",
    .entry_language = .zig,
};

test "a Zig CPU1 entry is never a gcc translation unit" {
    const units = cpu1.sources(std.testing.allocator, middleware_app, zig_entry_image);
    defer std.testing.allocator.free(units);
    try std.testing.expectEqual(@as(usize, 1), units.len);
    try std.testing.expectEqualStrings("libs/ra8_hal/src/ra8_ipc.c", units[0]);
}

test "a Zig CPU1 entry is built for the M33 with the hard float ABI" {
    const q = cpu1.zig_target_query;
    try std.testing.expectEqual(std.Target.Cpu.Arch.thumb, q.cpu_arch.?);
    try std.testing.expectEqual(std.Target.Abi.eabihf, q.abi.?);
    try std.testing.expectEqualStrings("cortex_m33", q.cpu_model.explicit.name);
}

test "threadx_cpu1, the bare CPU1 examples and the Module Manager apps are the Zig CPU1 entries" {
    // A null kernel is a bare-metal CPU1 half that owns its own vector table.
    const expected = [_]struct { app: []const u8, kernel: ?[]const u8 }{
        .{ .app = "threadx_cpu1", .kernel = "threadx_m33" },
        .{ .app = "txm_manager_cpu1", .kernel = "threadx_m33_modules" },
        .{ .app = "txm_fault_cpu1", .kernel = "threadx_m33_modules" },
        .{ .app = "txm_table_cpu1", .kernel = "threadx_m33_modules" },
        .{ .app = "cpu1_pingpong_ra8p1", .kernel = null },
        .{ .app = "txm_rpc_cpu1", .kernel = "threadx_m33_modules" },
        .{ .app = "txm_reload_cpu1", .kernel = "threadx_m33_modules" },
        .{ .app = "cpu1_routed_irq", .kernel = null },
    };
    var zig_entries: usize = 0;
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        if (image.entry_language == .c) {
            try std.testing.expectEqual(@as(usize, 0), image.uses.len);
            try std.testing.expect(image.txm_module == null);
            continue;
        }
        try std.testing.expect(zig_entries < expected.len);
        try std.testing.expectEqualStrings(expected[zig_entries].app, app.name);
        if (expected[zig_entries].kernel) |kernel| {
            try std.testing.expectEqual(@as(usize, 1), image.uses.len);
            try std.testing.expectEqualStrings(kernel, image.uses[0]);
        } else {
            try std.testing.expectEqual(@as(usize, 0), image.uses.len);
        }
        zig_entries += 1;
    }
    try std.testing.expectEqual(expected.len, zig_entries);
}

test "only the Module Manager images carry a packed module, each in its own linker script" {
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        if (image.txm_module == null) continue;
        const module_apps = [_][]const u8{
            "txm_manager_cpu1", "txm_fault_cpu1", "txm_table_cpu1", "txm_rpc_cpu1",
            "txm_reload_cpu1",
        };
        var known = false;
        for (module_apps) |name| known = known or std.mem.eql(u8, app.name, name);
        try std.testing.expect(known);
        try std.testing.expectEqualStrings("linker_script_cpu1.ld", image.linker_script);
    }
}

test "an M33 image without its own linker script falls back to its board layer (RA8FW-496)" {
    const allocator = std.testing.allocator;
    const ek = cpu1.boardLinkerScript(allocator, "libs/ra8_board_ek_ra8d2", "linker_script_cpu1.ld");
    defer allocator.free(ek);
    try std.testing.expectEqualStrings("libs/ra8_board_ek_ra8d2/ld/linker_script_cpu1.ld", ek);
    const ra8p1 = cpu1.boardLinkerScript(allocator, "libs/ra8_board_ra8p1", "linker_script_cpu1.ld");
    defer allocator.free(ra8p1);
    try std.testing.expectEqualStrings("libs/ra8_board_ra8p1/ld/linker_script_cpu1.ld", ra8p1);
    // Both fallbacks are real files in the tree.
    try std.fs.cwd().access(ek, .{});
    try std.fs.cwd().access(ra8p1, .{});
}

test "the RA8P1 ping-pong pair links CPU1 from the RA8P1 board layer (RA8FW-496)" {
    const allocator = std.testing.allocator;
    var found = false;
    for (graph.cross_apps) |app| {
        if (!std.mem.eql(u8, app.name, "cpu1_pingpong_ra8p1")) continue;
        found = true;
        const image = app.cpu1 orelse return error.NoCpu1Image;
        try std.testing.expect(app.cpu1_image);
        try std.testing.expectEqualStrings("libs/ra8_board_ra8p1", app.board);
        try std.testing.expectEqual(.zig, image.entry_language);
        try std.testing.expectEqual(@as(usize, 0), image.uses.len);
        const script = cpu1.boardLinkerScript(allocator, app.board, image.linker_script);
        defer allocator.free(script);
        try std.testing.expectEqualStrings("libs/ra8_board_ra8p1/ld/linker_script_cpu1.ld", script);
    }
    try std.testing.expect(found);
}

test "only cpu1_pingpong_ipc's M33 image links a Zig library beyond ra8_core (RA8FW-572)" {
    var found = false;
    for (graph.cross_apps) |app| {
        const image = app.cpu1 orelse continue;
        if (!std.mem.eql(u8, app.name, "cpu1_pingpong_ipc")) {
            try std.testing.expectEqual(@as(usize, 0), image.zig_libraries.len);
            continue;
        }
        found = true;
        try std.testing.expectEqual(@as(usize, 1), image.zig_libraries.len);
        try std.testing.expectEqualStrings("ra8_hal", image.zig_libraries[0]);
    }
    try std.testing.expect(found);
}
