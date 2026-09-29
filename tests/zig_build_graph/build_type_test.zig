//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The configurations build_type.zig declares, held against the REAL root
//! CMakeLists.txt (#1179, part of #857).
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
const build_type = graph.build_type;

const root_cmakelists_source = @embedFile("root_cmakelists_source");

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
    // configuration all over again, and #859 cannot retire a build system over
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
