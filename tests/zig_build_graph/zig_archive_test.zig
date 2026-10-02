//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The archive optimisation rules, held against the REAL
//! cmake/ra8_app/zig_libs.cmake and the REAL cross-image wiring in
//! tests/zig_build_graph/cross_image.zig (part of RA8FW-339).
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
const cross_image_source = @embedFile("cross_image_source");

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

test "the listfile answers every configuration, from the knob it documents" {
    const allocator = std.testing.allocator;
    const listfile = try mapping(allocator);
    defer allocator.free(listfile.named);

    // Until the single-mode change this rule read the other way round: the listfile branched on
    // CMAKE_BUILD_TYPE, and a mapping that stopped varying was the defect to
    // catch. It now holds one mode for every configure deliberately, so what
    // is worth refusing is a listfile that answers SOME configuration
    // differently without the graph's table having moved with it.
    for (build_type.configurations) |configuration| {
        const declared = listfile.forName(configuration.cmake_name);
        if (declared != listfile.forName(build_type.configurations[0].cmake_name)) {
            std.debug.print(
                "zig_libs.cmake answers {s} with {s} but {s} with {s};" ++
                    " the single-mode rule introduced no longer holds\n",
                .{
                    configuration.cmake_name,
                    @tagName(declared),
                    build_type.configurations[0].cmake_name,
                    @tagName(listfile.forName(build_type.configurations[0].cmake_name)),
                },
            );
            return error.ArchiveOptimizationVaries;
        }
    }

    // And refuse to report clean against nothing: the answer has to be a mode
    // this graph spells, reached through the documented knob rather than
    // guessed. RA8_ZIG_OPTIMIZE's own default is what a configure with no -D
    // override gets, so that is the value the rule above is comparing.
    const knob = zig_archive.cacheDefault(zig_libs_cmake_source, "RA8_ZIG_OPTIMIZE") orelse {
        std.debug.print(
            "zig_libs.cmake no longer declares RA8_ZIG_OPTIMIZE as a cache variable;" ++
                " nothing documents what a migrated archive is built at\n",
            .{},
        );
        return error.NoOptimizeKnobInListfile;
    };
    try std.testing.expectEqual(
        zig_archive.optimizeFromName(knob).?,
        listfile.forName("RelWithDebInfo"),
    );
}

test "the cross-build asks for the selected configuration, not a fixed mode" {
    const argument = zig_archive.archiveOptimizeArgument(cross_image_source) orelse {
        std.debug.print(
            "cross_image.zig has no .optimize on the archive request in its" ++
                " for (app.zig_libraries) loop; nothing pins what a migrated" ++
                " archive is built at\n",
            .{},
        );
        return error.NoArchiveRequestInGraph;
    };
    if (!zig_archive.isConfigurationDriven(argument)) {
        std.debug.print(
            "cross_image.zig asks for a migrated archive at `{s}`, a mode pinned regardless of" ++
                " -Dbuild-type; at two of CMake's three configurations that is the wrong artifact\n",
            .{argument},
        );
        return error.ArchiveOptimizationPinned;
    }
}

test "an app in the table actually names a migrated library" {
    // Without one, every rule above is about a code path no app reaches, and
    // the zig_libraries hook reads as wiring that is never exercised (which is
    // exactly what it was, until the unwind-table link failure was fixed).
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
