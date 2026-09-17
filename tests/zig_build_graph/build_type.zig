//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The three build configurations the root CMakeLists declares, and the global
//! flag sets each one produces (#1179).
//!
//! Every slice of this graph before #1179 hard-coded Debug: `-O0 -g3 -DDEBUG`
//! was spliced into the compile step, the link step, both second images and
//! every compile-database row, and nothing could ask for anything else. Debug
//! is also the configuration almost nothing in the repo asks for. CI's own
//! gate (scripts/ci/gates/build.sh), scripts/builders/all_examples.sh,
//! scripts/builders/build_cross_compile_db.py and the `ra8d2-release` preset
//! all configure RelWithDebInfo.
//!
//! A missed configuration is silent: the graph stays green, the images link,
//! and they are Debug images wearing another configuration's name. `-DNDEBUG`
//! alone turns every `assert()` into nothing.
//!
//! This module is a LEAF on purpose, the way off_target.zig is. It knows the
//! configurations and nothing about the toolchain file, so the sets that do
//! NOT vary by configuration are handed in by the caller as a `Base`. That is
//! also what lets the whole thing be a pure function with fixture tests, and
//! what lets build_type_test.zig hold these three records against the real
//! root CMakeLists.txt rather than against a copy of it.

const std = @import("std");

/// The configurations the root listfile spells. CMake accepts any string for
/// CMAKE_BUILD_TYPE; these are the three that carry flags here.
pub const BuildType = enum { debug, release, relwithdebinfo };

/// One configuration, as the root CMakeLists declares it.
pub const Configuration = struct {
    build_type: BuildType,
    /// The CMAKE_BUILD_TYPE spelling, which is what a `set(CMAKE_C_FLAGS_<X>)`
    /// name is derived from and what a configure is asked for by.
    cmake_name: []const u8,
    /// CMAKE_C_FLAGS_<CONFIG>. Spliced after the toolchain file's own set and
    /// before the dialect, which is where CMake composes it.
    c_flags: []const []const u8,
    /// CMAKE_ASM_FLAGS_<CONFIG>. NOT the same set: the assembler is handed the
    /// debug level alone, no optimisation level and no NDEBUG, so a middleware
    /// port's `.S` units differ from the C units beside them in exactly this.
    asm_flags: []const []const u8,
    /// What cmake/ra8_app/zig_libs.cmake maps this configuration onto when it
    /// builds a migrated Zig archive: Debug stays Debug, everything else is
    /// ReleaseSmall. An archive built at a different optimisation than the
    /// objects beside it is not the artifact CMake links.
    zig_optimize: std.builtin.OptimizeMode,
};

/// Measured from the root CMakeLists, and held to it by build_type_test.zig.
pub const configurations = [_]Configuration{
    .{
        .build_type = .debug,
        .cmake_name = "Debug",
        .c_flags = &.{ "-O0", "-g3", "-DDEBUG" },
        .asm_flags = &.{"-g3"},
        .zig_optimize = .Debug,
    },
    .{
        .build_type = .release,
        .cmake_name = "Release",
        .c_flags = &.{ "-Os", "-g1", "-DNDEBUG" },
        .asm_flags = &.{"-g1"},
        .zig_optimize = .ReleaseSmall,
    },
    .{
        // The one CI actually gates on, and the only one of the three whose C
        // flags carry NEITHER -DDEBUG nor -DNDEBUG: assert() stays live in an
        // optimised image.
        .build_type = .relwithdebinfo,
        .cmake_name = "RelWithDebInfo",
        .c_flags = &.{ "-Og", "-g3" },
        .asm_flags = &.{"-g3"},
        .zig_optimize = .ReleaseSmall,
    },
};

/// The configuration for a build type. Total by construction: a new enum tag
/// with no record here fails the exhaustiveness test below rather than
/// silently defaulting to Debug.
pub fn forType(build_type: BuildType) Configuration {
    for (configurations) |configuration| {
        if (configuration.build_type == build_type) return configuration;
    }
    std.debug.panic("ra8: no configuration declared for build type {s}", .{@tagName(build_type)});
}

/// A CMAKE_BUILD_TYPE spelling, as `-Dbuild-type=` hands it over. Matched
/// case-insensitively because CMake itself compares the string exactly but
/// every caller in the repo spells it in mixed case; an unrecognised name is
/// null so the caller can refuse it by name rather than build the wrong thing.
pub fn parse(name: []const u8) ?BuildType {
    for (configurations) |configuration| {
        if (std.ascii.eqlIgnoreCase(name, configuration.cmake_name)) return configuration.build_type;
    }
    return null;
}

/// Every CMAKE_BUILD_TYPE this graph accepts, comma-joined, for the option
/// description and for the error a bad name gets.
pub fn names(allocator: std.mem.Allocator) []const u8 {
    var out = std.ArrayList(u8).init(allocator);
    for (configurations, 0..) |configuration, index| {
        if (index != 0) out.appendSlice(", ") catch @panic("OOM");
        out.appendSlice(configuration.cmake_name) catch @panic("OOM");
    }
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The sets that do NOT vary by configuration, handed in rather than imported
/// so this module stays a leaf. Each field is the CMake variable named in its
/// comment, with the configuration's own set composed into the middle of it.
pub const Base = struct {
    /// CMAKE_C_FLAGS from the toolchain file (the CPU selection and the
    /// section splitting).
    c_flags: []const []const u8,
    /// What CMake appends AFTER the configuration's set on a C command line:
    /// the standard selection the root listfile sets.
    c_dialect: []const []const u8,
    /// CMAKE_ASM_FLAGS, which carries the CPU selection alone.
    asm_flags: []const []const u8,
    /// CMAKE_EXE_LINKER_FLAGS, which trails the C flags on a link line.
    link_flags: []const []const u8,
};

/// The four global sets a cross build hands out, at one configuration.
/// Assembled once per invocation so the `arm` step, the second images and the
/// compile-database rows cannot drift apart about which configuration they are.
pub const Globals = struct {
    configuration: Configuration,
    /// CMAKE_C_FLAGS + CMAKE_C_FLAGS_<CONFIG> + the dialect: what every C
    /// translation unit in the configure inherits, app target or not.
    c_flags: []const []const u8,
    /// CMAKE_ASM_FLAGS + CMAKE_ASM_FLAGS_<CONFIG>.
    asm_flags: []const []const u8,
    /// The configuration's own set alone, for the call sites that splice it
    /// between the CPU flags and the dialect themselves.
    config_flags: []const []const u8,
    /// What a link line carries: the CPU flags, the configuration, then the
    /// linker flags. CMake puts CMAKE_C_FLAGS_<CONFIG> on the link too.
    link_flags: []const []const u8,
};

pub fn globals(allocator: std.mem.Allocator, build_type: BuildType, base: Base) Globals {
    const configuration = forType(build_type);
    return .{
        .configuration = configuration,
        .c_flags = join(allocator, &.{ base.c_flags, configuration.c_flags, base.c_dialect }),
        .asm_flags = join(allocator, &.{ base.asm_flags, configuration.asm_flags }),
        .config_flags = configuration.c_flags,
        .link_flags = join(allocator, &.{ base.c_flags, configuration.c_flags, base.link_flags }),
    };
}

fn join(allocator: std.mem.Allocator, sets: []const []const []const u8) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    for (sets) |set| out.appendSlice(set) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The flag list of one `set(CMAKE_<LANG>_FLAGS_<CONFIG> "...")` call in a
/// CMake listfile, whitespace-split, or null when the listfile has no such
/// call. Reading the real text is the point: a table of flags copied out of a
/// listfile agrees with it exactly once, on the day it was copied.
pub fn cmakeFlags(allocator: std.mem.Allocator, source: []const u8, variable: []const u8) ?[]const []const u8 {
    const needle = std.fmt.allocPrint(allocator, "set({s}", .{variable}) catch @panic("OOM");
    var lines = std.mem.splitScalar(u8, source, '\n');
    while (lines.next()) |line| {
        const text = std.mem.trim(u8, line, " \t\r");
        if (!std.mem.startsWith(u8, text, needle)) continue;
        const open = std.mem.indexOfScalar(u8, text, '"') orelse continue;
        const close = std.mem.lastIndexOfScalar(u8, text, '"') orelse continue;
        if (close <= open) continue;
        var out = std.ArrayList([]const u8).init(allocator);
        var flags = std.mem.tokenizeAny(u8, text[open + 1 .. close], " \t");
        while (flags.next()) |flag| out.append(flag) catch @panic("OOM");
        return out.toOwnedSlice() catch @panic("OOM");
    }
    return null;
}

test "every build type has exactly one configuration" {
    inline for (@typeInfo(BuildType).@"enum".fields) |field| {
        const build_type: BuildType = @enumFromInt(field.value);
        var seen: usize = 0;
        for (configurations) |configuration| {
            if (configuration.build_type == build_type) seen += 1;
        }
        try std.testing.expectEqual(@as(usize, 1), seen);
    }
}

test "the assembler set is not the C set" {
    for (configurations) |configuration| {
        // The debug level and nothing else: no optimisation level, no NDEBUG.
        try std.testing.expectEqual(@as(usize, 1), configuration.asm_flags.len);
        try std.testing.expect(std.mem.startsWith(u8, configuration.asm_flags[0], "-g"));
        try std.testing.expect(configuration.c_flags.len >= 2);
    }
}

test "only Debug builds a Debug Zig archive" {
    for (configurations) |configuration| {
        const expected: std.builtin.OptimizeMode = if (configuration.build_type == .debug)
            .Debug
        else
            .ReleaseSmall;
        try std.testing.expectEqual(expected, configuration.zig_optimize);
    }
}

test "RelWithDebInfo carries neither DEBUG nor NDEBUG" {
    for (forType(.relwithdebinfo).c_flags) |flag| {
        try std.testing.expect(!std.mem.eql(u8, flag, "-DDEBUG"));
        try std.testing.expect(!std.mem.eql(u8, flag, "-DNDEBUG"));
    }
}

test "a name is parsed the way a configure spells it, and a wrong one is refused" {
    try std.testing.expectEqual(BuildType.relwithdebinfo, parse("RelWithDebInfo").?);
    try std.testing.expectEqual(BuildType.relwithdebinfo, parse("relwithdebinfo").?);
    try std.testing.expectEqual(BuildType.debug, parse("Debug").?);
    try std.testing.expectEqual(BuildType.release, parse("Release").?);
    try std.testing.expect(parse("MinSizeRel") == null);
    try std.testing.expect(parse("") == null);
}

test "globals compose the configuration where CMake composes it" {
    const base = Base{
        .c_flags = &.{ "-mcpu=cortex-m85", "-ffunction-sections" },
        .c_dialect = &.{"-std=gnu2x"},
        .asm_flags = &.{"-mcpu=cortex-m85"},
        .link_flags = &.{"-nostdlib"},
    };
    const g = globals(std.testing.allocator, .relwithdebinfo, base);
    defer {
        std.testing.allocator.free(g.c_flags);
        std.testing.allocator.free(g.asm_flags);
        std.testing.allocator.free(g.link_flags);
    }
    try std.testing.expectEqualDeep(
        @as([]const []const u8, &.{ "-mcpu=cortex-m85", "-ffunction-sections", "-Og", "-g3", "-std=gnu2x" }),
        g.c_flags,
    );
    try std.testing.expectEqualDeep(@as([]const []const u8, &.{ "-mcpu=cortex-m85", "-g3" }), g.asm_flags);
    try std.testing.expectEqualDeep(
        @as([]const []const u8, &.{ "-mcpu=cortex-m85", "-ffunction-sections", "-Og", "-g3", "-nostdlib" }),
        g.link_flags,
    );
}

test "a listfile's own flag line is read, and an absent one is null" {
    const source =
        \\# a comment
        \\set(CMAKE_C_FLAGS_RELEASE "-Os -g1 -DNDEBUG")
        \\  set(CMAKE_ASM_FLAGS_RELEASE "-g1")
        \\
    ;
    try std.testing.expectEqualDeep(
        @as(?[]const []const u8, &.{ "-Os", "-g1", "-DNDEBUG" }),
        cmakeFlags(std.testing.allocator, source, "CMAKE_C_FLAGS_RELEASE"),
    );
    const asm_flags = cmakeFlags(std.testing.allocator, source, "CMAKE_ASM_FLAGS_RELEASE").?;
    defer std.testing.allocator.free(asm_flags);
    try std.testing.expectEqual(@as(usize, 1), asm_flags.len);
    try std.testing.expect(cmakeFlags(std.testing.allocator, source, "CMAKE_C_FLAGS_MINSIZEREL") == null);
}
