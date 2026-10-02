//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! What a migrated Zig library's ARCHIVE is built at when an app links it, and
//! the two places that answer has to agree (part of RA8FW-339).
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
//! then names the missing symbols. The OPTIMISATION is the silent half: a
//! graph that builds a migrated archive at one mode while CMake builds it at
//! another links a perfectly good image that is simply not the artifact CMake
//! produces, and nothing fails.
//!
//! So the mapping is read out of that listfile's own text rather than copied
//! into a table here. A table agrees with a listfile exactly once, on the day
//! it was copied, which is precisely what happened when the archive mode
//! became one cache variable: the listfile
//! stopped branching on CMAKE_BUILD_TYPE and started reading one cache
//! variable, every configuration now builds ReleaseSmall unless a configure
//! passes -DRA8_ZIG_OPTIMIZE=, and the rules here went on asserting the old
//! shape until the gate was fixed. Reading the text is what makes that a failing test
//! rather than a quiet disagreement; following one level of variable
//! indirection is what makes it readable at all.

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

/// The variable name a `${NAME}` reference wraps, or null when the text is
/// not one. CMake dereferences at use; this graph reads text, so it has to.
fn referencedVariable(value: []const u8) ?[]const u8 {
    if (!std.mem.startsWith(u8, value, "${")) return null;
    if (!std.mem.endsWith(u8, value, "}")) return null;
    const name = value[2 .. value.len - 1];
    return if (name.len == 0) null else name;
}

/// The default of a `set(<name> "<value>" CACHE <type> "<doc>")` declaration,
/// read across however many lines the formatter broke it over. Null when the
/// listfile has no cache declaration of that name, so a reference this graph
/// cannot resolve stays as loud as a mode it cannot spell.
pub fn cacheDefault(source: []const u8, name: []const u8) ?[]const u8 {
    var buffer: [128]u8 = undefined;
    const needle = std.fmt.bufPrint(&buffer, "set({s}", .{name}) catch return null;
    const at = std.mem.indexOf(u8, source, needle) orelse return null;
    const rest = source[at + needle.len ..];
    // The declaration ends at its own close paren: no nested call appears
    // between a cache variable's name and its doc string.
    const close = std.mem.indexOfScalar(u8, rest, ')') orelse return null;
    const body = rest[0..close];
    // Without CACHE this is an ordinary assignment, which a caller resolving a
    // configure-time knob must not read as one.
    if (std.mem.indexOf(u8, body, "CACHE") == null) return null;
    const open_quote = std.mem.indexOfScalar(u8, body, '"') orelse return null;
    const after = body[open_quote + 1 ..];
    const close_quote = std.mem.indexOfScalar(u8, after, '"') orelse return null;
    const value = std.mem.trim(u8, after[0..close_quote], " \t");
    return if (value.len == 0) null else value;
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
        const raw = setValue(text, variable) orelse continue;
        // `set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")` is the shape the single-mode change left
        // behind: the answer is the knob's own default, one hop away in the
        // same listfile.
        const value = if (referencedVariable(raw)) |name|
            cacheDefault(source, name) orelse return null
        else
            raw;
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

/// The value the cross-build graph passes as `.optimize` when it asks a
/// migrated library's build.zig for an ARM archive, read out of the wiring's
/// own text: the argument of the `.optimize =` line inside the
/// `for (app.zig_libraries)` loop of cross_image.addCrossApp(). Null when that
/// loop or that line is no longer there, which the rule below refuses.
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
/// exact defect this rule fixes: it builds the archive CMake produces at ONE of
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

test "a set through a cache knob resolves to the knob's default" {
    // The shape the single-mode change left in zig_libs.cmake, formatter line breaks and all.
    const source =
        \\set(RA8_ZIG_OPTIMIZE
        \\    "ReleaseSmall"
        \\    CACHE STRING "zig -Doptimize mode for the ported libraries"
        \\)
        \\function(_ra8_zig_build_archive _lib)
        \\  set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")
        \\endfunction()
        \\
    ;
    const mapping = parse(std.testing.allocator, source, "_zig_optimize").?;
    defer std.testing.allocator.free(mapping.named);
    try std.testing.expectEqual(@as(usize, 0), mapping.named.len);
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("Debug"));
    try std.testing.expectEqual(std.builtin.OptimizeMode.ReleaseSmall, mapping.forName("RelWithDebInfo"));
}

test "a reference this graph cannot resolve is null, not a guess" {
    // A knob that is not declared in this listfile at all.
    const dangling = "set(_zig_optimize \"${RA8_ZIG_OPTIMIZE}\")\n";
    try std.testing.expect(parse(std.testing.allocator, dangling, "_zig_optimize") == null);

    // Declared, but as a plain assignment rather than a configure-time knob:
    // not what a -D override reaches, so not an answer about one.
    const not_cached =
        \\set(RA8_ZIG_OPTIMIZE "ReleaseSmall")
        \\set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")
        \\
    ;
    try std.testing.expect(parse(std.testing.allocator, not_cached, "_zig_optimize") == null);

    // Declared as a knob, defaulted to a mode this graph does not spell.
    const unknown_default =
        \\set(RA8_ZIG_OPTIMIZE "ReleaseTurbo" CACHE STRING "doc")
        \\set(_zig_optimize "${RA8_ZIG_OPTIMIZE}")
        \\
    ;
    try std.testing.expect(parse(std.testing.allocator, unknown_default, "_zig_optimize") == null);
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
