//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The archive optimisation rules, held against the REAL
//! cmake/ra8_app/zig_libs.cmake and the REAL build.zig (#1244, part of #857).
//!
//! Both files arrive as anonymous imports declared in build.zig, so they are
//! read at COMPILE time from the paths the build graph itself names. A test
//! that opened them through std.fs would be asserting something about the
//! working directory it happened to run in, and would pass vacuously wherever
//! that guess was wrong.

const std = @import("std");
const graph = @import("build_graph");
const zig_archive = graph.zig_archive;
const build_type = graph.build_type;

const zig_libs_cmake_source = @embedFile("zig_libs_cmake_source");
const build_zig_source = @embedFile("build_zig_source");

/// The variable zig_libs.cmake holds the answer in. Named here rather than in
/// the module so a rename shows up as one failing line with this text next to
/// it, not as a silently empty mapping.
const optimize_variable = "_zig_optimize";

fn mapping(allocator: std.mem.Allocator) !zig_archive.Mapping {
    return zig_archive.parse(allocator, zig_libs_cmake_source, optimize_variable) orelse {
        std.debug.print(
            "cmake/ra8_app/zig_libs.cmake carries no set({s} ...) mapping this graph can read;" ++
                " the archive optimisation rules would be holding the graph to nothing\n",
            .{optimize_variable},
        );
        return error.NoMappingInListfile;
    };
}

test "every configuration builds the archive CMake's own rule asks for" {
    const allocator = std.testing.allocator;
    const listfile = try mapping(allocator);
    defer allocator.free(listfile.named);

    for (build_type.configurations) |configuration| {
        const declared = listfile.forName(configuration.cmake_name);
        if (declared != configuration.zig_optimize) {
            std.debug.print(
                "at {s}, zig_libs.cmake builds a migrated archive {s} but the graph asks for {s}\n",
                .{ configuration.cmake_name, @tagName(declared), @tagName(configuration.zig_optimize) },
            );
            return error.ArchiveOptimizationDisagrees;
        }
    }
}

test "the listfile's mapping still varies by configuration" {
    const allocator = std.testing.allocator;
    const listfile = try mapping(allocator);
    defer allocator.free(listfile.named);

    // Refuse to report clean against nothing. The rule above passes just as
    // well against a listfile that stopped branching, because the graph would
    // then be agreeing with a constant; one named arm plus an else is the
    // shape that makes it a real comparison.
    try std.testing.expect(listfile.named.len >= 1);
    var varies = false;
    for (listfile.named) |arm| {
        if (arm.optimize != listfile.fallback) varies = true;
    }
    try std.testing.expect(varies);
}

test "the cross-build asks for the selected configuration, not a fixed mode" {
    const argument = zig_archive.archiveOptimizeArgument(build_zig_source) orelse {
        std.debug.print(
            "build.zig has no .optimize on the archive request in its" ++
                " for (app.zig_libraries) loop; nothing pins what a migrated archive is built at\n",
            .{},
        );
        return error.NoArchiveRequestInGraph;
    };
    if (!zig_archive.isConfigurationDriven(argument)) {
        std.debug.print(
            "build.zig asks for a migrated archive at `{s}`, a mode pinned regardless of" ++
                " -Dbuild-type; at two of CMake's three configurations that is the wrong artifact\n",
            .{argument},
        );
        return error.ArchiveOptimizationPinned;
    }
}

test "an app in the table actually names a migrated library" {
    // Without one, every rule above is about a code path no app reaches, and
    // the zig_libraries hook reads as wiring that is never exercised (which is
    // exactly what it was, from #936 until #948 was closed).
    var apps_with_archives: usize = 0;
    for (graph.cross_apps) |app| {
        if (app.zig_libraries.len == 0) continue;
        apps_with_archives += 1;
        for (app.zig_libraries) |lib_name| {
            // A migrated library named in `zig_libraries` has to be named in
            // `libraries` too: zig_libs.cmake only ever sees the LIBS list.
            var in_libs = false;
            for (app.libraries) |library| {
                if (std.mem.eql(u8, library, lib_name)) in_libs = true;
            }
            if (!in_libs) {
                std.debug.print(
                    "{s} cross-builds the {s} archive but does not name it in LIBS\n",
                    .{ app.name, lib_name },
                );
                return error.ArchiveNotInLibs;
            }
        }
    }
    try std.testing.expect(apps_with_archives >= 1);
}
