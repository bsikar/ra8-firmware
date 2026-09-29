//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a migrated Zig library's ARCHIVE is built at when an app links it, and
//! the two places that answer has to agree (#1244, part of #857).
//!
//! An app whose `LIBS` names a migrated library takes the one arm of
//! ra8_add_app() no earlier slice could: the library keeps its public `inc/`
//! header and drops `src/*.c`, so the LIBS glob in cmake/ra8_app/sources.cmake
//! finds nothing to compile for it, and cmake/ra8_app/zig_libs.cmake
//! cross-builds `libs/<lib>/build.zig` for the app's own core and links the
//! static archive that comes out.
//!
//! Losing the archive fails closed, so it needs no rule here: every one of the
//! app's own units still compiles against the unchanged header, and the link
//! then names the missing symbols. The OPTIMISATION is the silent half.
//! zig_libs.cmake maps a Debug configure onto a Debug archive and every other
//! configure onto ReleaseSmall; a graph that picks one of the two and keeps it
//! links a perfectly good image that is simply not the artifact CMake
//! produces, at three of the repo's four configure sites.
//!
//! So the mapping is read out of that listfile's own text rather than copied
//! into a table here. A table agrees with a listfile exactly once, on the day
//! it was copied.

const std = @import("std");

/// One `if(CMAKE_BUILD_TYPE STREQUAL "<name>")` arm of the mapping.
pub const NamedOptimize = struct {
    /// The CMAKE_BUILD_TYPE spelling the listfile compares against.
    cmake_name: []const u8,
    optimize: std.builtin.OptimizeMode,
};

/// The whole mapping zig_libs.cmake applies, as its text spells it.
pub const Mapping = struct {
    /// The configurations it tests by name, in listfile order.
    named: []const NamedOptimize,
    /// The `else()` arm: what every configuration it does NOT name gets.
    /// Absent only if the listfile stopped having one, which `parse` refuses.
    fallback: std.builtin.OptimizeMode,

    /// What a configure of `cmake_name` gets a migrated archive built at.
    pub fn forName(self: Mapping, cmake_name: []const u8) std.builtin.OptimizeMode {
        for (self.named) |arm| {
            // CMake's STREQUAL is exact, so this is too.
            if (std.mem.eql(u8, arm.cmake_name, cmake_name)) return arm.optimize;
        }
        return self.fallback;
    }
};

/// The Zig optimisation mode a CMake listfile names, or null for a spelling
/// this graph does not know. Null rather than a default on purpose: a listfile
/// that started asking for ReleaseFast should fail loudly here, not be read as
/// whatever this file guesses.
pub fn optimizeFromName(name: []const u8) ?std.builtin.OptimizeMode {
    if (std.mem.eql(u8, name, "Debug")) return .Debug;
    if (std.mem.eql(u8, name, "ReleaseSmall")) return .ReleaseSmall;
    if (std.mem.eql(u8, name, "ReleaseSafe")) return .ReleaseSafe;
    if (std.mem.eql(u8, name, "ReleaseFast")) return .ReleaseFast;
    return null;
}

/// The single-token argument of a `set(<variable> <token>)` line, or null when
/// the line is not one. Quoted or bare, since CMake accepts both.
fn setValue(text: []const u8, variable: []const u8) ?[]const u8 {
    var buffer: [128]u8 = undefined;
    const needle = std.fmt.bufPrint(&buffer, "set({s} ", .{variable}) catch return null;
    if (!std.mem.startsWith(u8, text, needle)) return null;
    const close = std.mem.lastIndexOfScalar(u8, text, ')') orelse return null;
    if (close <= needle.len) return null;
    return std.mem.trim(u8, text[needle.len..close], " \t\"");
}

/// The quoted string of an `if(CMAKE_BUILD_TYPE STREQUAL "<name>")` line, or
/// null when the line does not test the build type by name.
fn buildTypeArm(text: []const u8) ?[]const u8 {
    const prefix = "if(CMAKE_BUILD_TYPE STREQUAL";
    if (!std.mem.startsWith(u8, text, prefix)) return null;
    const open = std.mem.indexOfScalar(u8, text, '"') orelse return null;
    const close = std.mem.lastIndexOfScalar(u8, text, '"') orelse return null;
    if (close <= open) return null;
    return text[open + 1 .. close];
}

/// Read the optimisation mapping out of a listfile's own text. Null when the
/// listfile carries no mapping this can see, when it names a mode this graph
/// does not know, or when it has no `else()` arm left: each of those means the
/// rules below would be holding the graph to nothing, and reporting clean
/// against nothing is the failure this whole slice exists to prevent.
pub fn parse(allocator: std.mem.Allocator, source: []const u8, variable: []const u8) ?Mapping {
    var named = std.ArrayList(NamedOptimize).init(allocator);
    var fallback: ?std.builtin.OptimizeMode = null;
    // Which arm the walk is inside: a name it matched, or the else().
    var arm: ?[]const u8 = null;
    var in_else = false;

    var lines = std.mem.splitScalar(u8, source, '\n');
    while (lines.next()) |line| {
        const text = std.mem.trim(u8, line, " \t\r");
        if (buildTypeArm(text)) |name| {
            arm = name;
            in_else = false;
            continue;
        }
        if (std.mem.eql(u8, text, "else()")) {
            in_else = true;
            continue;
        }
        if (std.mem.eql(u8, text, "endif()")) {
            arm = null;
            in_else = false;
            continue;
        }
        const value = setValue(text, variable) orelse continue;
        const mode = optimizeFromName(value) orelse return null;
        if (in_else) {
            fallback = mode;
        } else if (arm) |name| {
            named.append(.{ .cmake_name = name, .optimize = mode }) catch @panic("OOM");
        } else {
            // An unconditional set: the mapping stopped varying at all, which
            // is a real answer for every configuration.
            fallback = mode;
        }
    }

    if (named.items.len == 0 and fallback == null) return null;
    return .{
        .named = named.toOwnedSlice() catch @panic("OOM"),
        .fallback = fallback orelse return null,
    };
}

/// The value the root build graph passes as `.optimize` when it asks a
/// migrated library's build.zig for an ARM archive, read out of build.zig's
/// own text: the argument of the `.optimize =` line inside the
/// `for (app.zig_libraries)` loop of addArmCrossApp(). Null when that loop or
/// that line is no longer there, which the rule below refuses.
pub fn archiveOptimizeArgument(source: []const u8) ?[]const u8 {
    const loop = "for (app.zig_libraries)";
    const start = std.mem.indexOf(u8, source, loop) orelse return null;
    const field = ".optimize = ";
    const at = std.mem.indexOfPos(u8, source, start, field) orelse return null;
    const rest = source[at + field.len ..];
    const end = std.mem.indexOfAny(u8, rest, ",\n") orelse return null;
    return std.mem.trim(u8, rest[0..end], " \t");
}

/// Report whether an `.optimize` argument is driven by the selected
/// configuration rather than pinned to one mode. A literal `.Debug` is the
/// exact defect #1244 fixes: it builds the archive CMake produces at ONE of
/// its three configurations and the wrong one at the other two, and nothing
/// fails.
pub fn isConfigurationDriven(argument: []const u8) bool {
    if (argument.len == 0) return false;
    // An enum literal is the whole of the pinned form; a configuration-driven
    // argument is a field access on the selected configuration.
    if (argument[0] == '.') return false;
    return std.mem.indexOf(u8, argument, "zig_optimize") != null;
}

test "a listfile's own mapping is read, arms and else alike" {
    const source =
        \\function(_ra8_app_zig_library _lib)
        \\  if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        \\    set(_zig_optimize Debug)
        \\  else()
        \\    set(_zig_optimize ReleaseSmall)
        \\  endif()
        \\endfunction()
        \\
    ;
    const mapping = parse(std.testing.allocator, source, "_zig_optimize").?;
    defer std.testing.allocator.free(mapping.named);
    try std.testing.expectEqual(@as(usize, 1), mapping.named.len);
    try std.testing.expectEqualStrings("Debug", mapping.named[0].cmake_name);
    try std.testing.expectEqual(std.builtin.OptimizeMode.Debug, mapping.named[0].optimize);
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.fallback);
    // The three the repo configures, through the accessor a caller uses.
    try std.testing.expectEqual(std.builtin.OptimizeMode.Debug, mapping.forName("Debug"));
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("Release"));
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("RelWithDebInfo"));
    // CMake's STREQUAL is exact, so a differently-cased spelling is NOT the
    // Debug arm, and falls to the else the way a real configure would.
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("debug"));
}

test "a mapping this graph cannot read is null, not a guess" {
    // No mapping at all.
    try std.testing.expect(parse(std.testing.allocator, "endfunction()\n", "_zig_optimize") == null);
    // A mode this graph does not know: loud, rather than read as Debug.
    const unknown =
        \\if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        \\  set(_zig_optimize Debug)
        \\else()
        \\  set(_zig_optimize ReleaseTurbo)
        \\endif()
        \\
    ;
    try std.testing.expect(parse(std.testing.allocator, unknown, "_zig_optimize") == null);
    // An arm with no else() left: every unnamed configuration would have no
    // answer, so there is nothing to hold the graph to.
    const no_else =
        \\if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        \\  set(_zig_optimize Debug)
        \\endif()
        \\
    ;
    try std.testing.expect(parse(std.testing.allocator, no_else, "_zig_optimize") == null);
}

test "an unconditional set answers for every configuration" {
    const source = "set(_zig_optimize ReleaseSmall)\n";
    const mapping = parse(std.testing.allocator, source, "_zig_optimize").?;
    defer std.testing.allocator.free(mapping.named);
    try std.testing.expectEqual(@as(usize, 0), mapping.named.len);
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("Debug"));
}

test "the archive request is read out of the loop that makes it" {
    const source =
        \\    for (app.zig_libraries) |lib_name| {
        \\        const dependency = b.dependency(lib_name, .{
        \\            .target = arm_target,
        \\            .optimize = arm.configuration.zig_optimize,
        \\        });
        \\    }
        \\
    ;
    try std.testing.expectEqualStrings(
        "arm.configuration.zig_optimize",
        archiveOptimizeArgument(source).?,
    );
    // The `.optimize` of some OTHER dependency, above the loop, is not this
    // one: the search starts at the loop.
    const decoy =
        \\const other = b.dependency("x", .{ .optimize = .ReleaseFast });
        \\for (app.zig_libraries) |lib_name| {
        \\    const dependency = b.dependency(lib_name, .{ .optimize = cfg.zig_optimize });
        \\}
        \\
    ;
    try std.testing.expectEqualStrings("cfg.zig_optimize", archiveOptimizeArgument(decoy).?);
    try std.testing.expect(archiveOptimizeArgument("fn addArmCrossApp() void {}\n") == null);
}

test "a pinned optimisation is not configuration-driven" {
    try std.testing.expect(isConfigurationDriven("arm.configuration.zig_optimize"));
    try std.testing.expect(!isConfigurationDriven(".Debug"));
    try std.testing.expect(!isConfigurationDriven(".ReleaseSmall"));
    try std.testing.expect(!isConfigurationDriven("optimize"));
    try std.testing.expect(!isConfigurationDriven(""));
}
