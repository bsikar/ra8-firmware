//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The configurations build_type.zig declares, held against the REAL root
//! CMakeLists.txt (part of RA8FW-339).
//!
//! The listfile arrives as an anonymous import declared in build.zig, so it is
//! read at COMPILE time from the path the build graph itself names. A test
//! that opened it through std.fs would be asserting something about the
//! working directory it happened to run in, and would pass vacuously wherever
//! that guess was wrong.
//!
//! In its own file rather than in build_type.zig because build.zig imports
//! that module directly: an @embedFile of an import name only the test module
//! declares cannot compile in the build runner.

const std = @import("std");
const graph = @import("build_graph");
const arm_flags = graph.arm_flags;
const build_type = graph.build_type;
const mram_budget_check = graph.cross_image.mram_budget_check;

const root_cmakelists_source = @embedFile("root_cmakelists_source");
const build_zig_source = @embedFile("build_zig_source");
const cmake_presets_source = @embedFile("cmake_presets_source");
const ereader_cmakelists_source = @embedFile("ereader_cmakelists_source");
const dfu_bootloader_cmakelists_source = @embedFile("dfu_bootloader_cmakelists_source");

test {
    std.testing.refAllDecls(build_type);
}

/// The CMake variable a configuration's flags are declared in, e.g.
/// CMAKE_C_FLAGS_RELWITHDEBINFO. CMake derives the suffix by upper-casing
/// CMAKE_BUILD_TYPE, which is exactly what this does.
fn variableFor(allocator: std.mem.Allocator, language: []const u8, cmake_name: []const u8) []const u8 {
    const upper = allocator.alloc(u8, cmake_name.len) catch @panic("OOM");
    _ = std.ascii.upperString(upper, cmake_name);
    return std.fmt.allocPrint(allocator, "CMAKE_{s}_FLAGS_{s}", .{ language, upper }) catch @panic("OOM");
}

test "each configuration's C flags are the listfile's own, in the listfile's order" {
    const allocator = std.testing.allocator;
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();

    for (build_type.configurations) |configuration| {
        const variable = variableFor(arena.allocator(), "C", configuration.cmake_name);
        const declared = build_type.cmakeFlags(arena.allocator(), root_cmakelists_source, variable) orelse {
            std.debug.print(
                "the root CMakeLists declares no {s}; build_type.zig claims a configuration CMake does not have\n",
                .{variable},
            );
            return error.ConfigurationNotInListfile;
        };
        try std.testing.expectEqualDeep(configuration.c_flags, declared);
    }
}

test "the assembler flags are the listfile's own too, and differ from the C set" {
    const allocator = std.testing.allocator;
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();

    for (build_type.configurations) |configuration| {
        const variable = variableFor(arena.allocator(), "ASM", configuration.cmake_name);
        const declared = build_type.cmakeFlags(arena.allocator(), root_cmakelists_source, variable) orelse
            return error.ConfigurationNotInListfile;
        try std.testing.expectEqualDeep(configuration.asm_flags, declared);
        // The reason they are separate fields: an assembly unit is handed the
        // debug level alone. A graph that gave a middleware's .S ports the C
        // set would build them at an optimisation CMake never used.
        try std.testing.expect(declared.len < configuration.c_flags.len);
    }
}

test "the graph knows every configuration the listfile declares flags for" {
    const allocator = std.testing.allocator;
    var arena = std.heap.ArenaAllocator.init(allocator);
    defer arena.deinit();

    // The other direction, which is the one that rots: a configuration added
    // to the listfile that the graph cannot build would be a CMake-only
    // configuration all over again, and RA8FW-332 cannot retire a build system over
    // one of those.
    var lines = std.mem.splitScalar(u8, root_cmakelists_source, '\n');
    var found: usize = 0;
    while (lines.next()) |line| {
        const text = std.mem.trim(u8, line, " \t\r");
        const prefix = "set(CMAKE_C_FLAGS_";
        if (!std.mem.startsWith(u8, text, prefix)) continue;
        const rest = text[prefix.len..];
        const end = std.mem.indexOfAny(u8, rest, " \t\"") orelse continue;
        const name = rest[0..end];
        found += 1;
        if (build_type.parse(name) == null) {
            std.debug.print("the root CMakeLists declares CMAKE_C_FLAGS_{s}, which the graph cannot build\n", .{name});
            return error.ListfileConfigurationNotInGraph;
        }
    }
    // Refuse to report clean against nothing: three configurations is what the
    // listfile carries, and a parser that suddenly matches none of them would
    // otherwise pass silently.
    try std.testing.expectEqual(build_type.configurations.len, found);
}

fn findCrossApp(name: []const u8) ?@TypeOf(graph.cross_apps[0]) {
    for (graph.cross_apps) |app| {
        if (std.mem.eql(u8, app.name, name)) return app;
    }
    return null;
}

fn containsFlag(flags: []const []const u8, needle: []const u8) bool {
    for (flags) |flag| {
        if (std.mem.eql(u8, flag, needle)) return true;
    }
    return false;
}

test "build.zig defaults -Dbuild-type to Debug and guards -Dproduction-build" {
    try std.testing.expect(std.mem.indexOf(u8, build_zig_source, "orelse \"Debug\";") != null);
    try std.testing.expect(std.mem.indexOf(u8, build_zig_source, "\"production-build\"") != null);
    try std.testing.expect(std.mem.indexOf(u8, build_zig_source, "validateProductionBuild") != null);

    try std.testing.expectEqual(build_type.BuildType.debug, build_type.default_build_type);
    try std.testing.expectEqual(build_type.BuildType.relwithdebinfo, build_type.shipping_build_type);
    try std.testing.expectEqual(std.lang.OptimizeMode.ReleaseSafe, build_type.release_tool_optimize);
    try std.testing.expect(!build_type.isShippingCapable(.debug));
    try std.testing.expect(build_type.isShippingCapable(.relwithdebinfo));
    try std.testing.expect(build_type.isShippingCapable(.release));
    try std.testing.expect(build_type.validateProductionBuild(true, .debug) != null);
    try std.testing.expect(build_type.validateProductionBuild(true, .relwithdebinfo) == null);
    try std.testing.expect(build_type.validateProductionBuild(true, .release) == null);
    try std.testing.expect(build_type.validateProductionBuild(false, .debug) == null);
}

test "shipping targets pin at least RelWithDebInfo while local targets keep Debug" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    const base = graph.cross_image.base();
    const debug_globals = build_type.globalsWithProduction(alloc, .debug, base, false);

    const dfu = findCrossApp("dfu_bootloader").?;
    const sb = findCrossApp("secure_boot_hil").?;
    const dev_app = findCrossApp("blink_hal").?;

    try std.testing.expectEqual(@as(?build_type.BuildType, .relwithdebinfo), dfu.min_build_type);
    try std.testing.expectEqual(@as(?build_type.BuildType, .relwithdebinfo), sb.min_build_type);
    try std.testing.expectEqual(@as(?build_type.BuildType, null), dev_app.min_build_type);

    const dfu_debug = build_type.globalsForTarget(alloc, debug_globals, dfu.min_build_type, base);
    try std.testing.expectEqual(build_type.BuildType.relwithdebinfo, dfu_debug.configuration.build_type);
    try std.testing.expectEqual(std.lang.OptimizeMode.ReleaseSmall, dfu_debug.configuration.zig_optimize);
    try std.testing.expect(containsFlag(dfu_debug.config_flags, "-Og"));
    try std.testing.expect(!containsFlag(dfu_debug.config_flags, "-DDEBUG"));

    const rel_globals = build_type.globalsWithProduction(alloc, .release, base, true);
    const dfu_release = build_type.globalsForTarget(alloc, rel_globals, dfu.min_build_type, base);
    try std.testing.expectEqual(build_type.BuildType.release, dfu_release.configuration.build_type);
    try std.testing.expect(dfu_release.production_build);
    try std.testing.expect(containsFlag(dfu_release.config_flags, "-Os"));

    const dev_debug = build_type.globalsForTarget(alloc, debug_globals, dev_app.min_build_type, base);
    try std.testing.expectEqual(build_type.BuildType.debug, dev_debug.configuration.build_type);
    try std.testing.expect(containsFlag(dev_debug.config_flags, "-O0"));

    try std.testing.expect(!containsFlag(arm_flags.globalDefines(false), arm_flags.production_define));
    try std.testing.expect(containsFlag(arm_flags.globalDefines(true), arm_flags.production_define));
}

fn declaresMramLength(cmake_source: []const u8, expected: []const u8) bool {
    var tokens = std.mem.tokenizeAny(u8, cmake_source, " \t\r\n()");
    while (tokens.next()) |tok| {
        if (std.mem.eql(u8, tok, "MRAM_LENGTH")) {
            const val = tokens.next() orelse return false;
            return std.mem.eql(u8, val, expected);
        }
    }
    return false;
}

test "MRAM budget parsing and verification align with CMakeLists declarations" {
    try std.testing.expectEqual(@as(?u32, 128 * 1024), build_type.parseMramBytes("128K"));
    try std.testing.expectEqual(@as(?u32, 512 * 1024), build_type.parseMramBytes("512K"));
    try std.testing.expectEqual(@as(?u32, 1024 * 1024), build_type.parseMramBytes("1M"));
    try std.testing.expect(build_type.parseMramBytes("") == null);
    try std.testing.expect(build_type.parseMramBytes("128G") == null);

    var mram_capped: usize = 0;
    for (graph.cross_apps) |app| {
        const spec = app.mram_length orelse continue;
        mram_capped += 1;
        try std.testing.expect(build_type.parseMramBytes(spec) != null);
    }
    try std.testing.expect(mram_capped >= 4);

    try std.testing.expect(declaresMramLength(dfu_bootloader_cmakelists_source, "128K"));
    try std.testing.expect(declaresMramLength(ereader_cmakelists_source, "512K"));

    try mram_budget_check.checkBudget("dfu_bootloader", 85_796, 128 * 1024);
    try std.testing.expectError(error.MramBudgetExceeded, mram_budget_check.checkBudget("dfu_bootloader", 128 * 1024 + 1, 128 * 1024));
    try std.testing.expectError(error.EmptyBinary, mram_budget_check.checkBudget("dfu_bootloader", 0, 128 * 1024));
}

test "CMakePresets release and shipping presets pin shipping build policy" {
    var arena = std.heap.ArenaAllocator.init(std.testing.allocator);
    defer arena.deinit();
    const alloc = arena.allocator();

    const parsed = try std.json.parseFromSlice(std.json.Value, alloc, cmake_presets_source, .{});
    const root = parsed.value.object;
    const configure_presets = root.get("configurePresets").?.array.items;
    const build_presets = root.get("buildPresets").?.array.items;

    var checked_configs: usize = 0;
    for (configure_presets) |entry| {
        const obj = entry.object;
        const name = obj.get("name").?.string;
        if (!std.mem.eql(u8, name, "ra8d2-release") and !std.mem.eql(u8, name, "ra8d2-shipping")) continue;
        checked_configs += 1;
        const vars = obj.get("cacheVariables").?.object;
        const bt_name = vars.get("CMAKE_BUILD_TYPE").?.string;
        const bt = build_type.parse(bt_name).?;
        try std.testing.expect(build_type.isShippingCapable(bt));
        try std.testing.expectEqualStrings("ON", vars.get("RA8_PRODUCTION_BUILD").?.string);
        try std.testing.expectEqualStrings("ReleaseSmall", vars.get("RA8_ZIG_OPTIMIZE").?.string);
    }
    try std.testing.expectEqual(@as(usize, 2), checked_configs);

    var found_ereader = false;
    for (build_presets) |entry| {
        const obj = entry.object;
        if (!std.mem.eql(u8, obj.get("name").?.string, "ra8d2-ereader-release")) continue;
        found_ereader = true;
        try std.testing.expectEqualStrings("ra8d2-release", obj.get("configurePreset").?.string);
        const targets = obj.get("targets").?.array.items;
        try std.testing.expectEqual(@as(usize, 2), targets.len);
        try std.testing.expectEqualStrings("ra8d2-ereader.elf", targets[0].string);
        try std.testing.expectEqualStrings("ra8d2-ereader_ns.elf", targets[1].string);
    }
    try std.testing.expect(found_ereader);
}
