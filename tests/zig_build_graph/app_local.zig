// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//
//! The part of an app's build that ra8_add_app() does not do.
//!
//! Most apps in this repo are one ra8_add_app() call and nothing else, so the
//! cross-build rules the graph has encoded so far (#936, #1021, #1036, #1054,
//! #1068) are all rules of that one function. cpu1_pingpong was the first app
//! whose own CMakeLists declares a second target (#1044). This module is the
//! general case of that: an app that declares a VENDORED STATIC LIBRARY beside
//! its executable, links it, and takes the library's PUBLIC usage
//! requirements -- defines and SYSTEM include directories -- onto every one of
//! its own translation units.
//!
//! Three of the four things that happens fail silently rather than loudly:
//!
//!   1. The library's own TUs are compiled at the library's bar, which is the
//!      toolchain flags plus whatever the app's CMakeLists names, and NOT the
//!      project warning profile: that profile rides on ra8_add_app()'s target,
//!      not on a separately-declared one. Compiling vendored crypto at
//!      -Werror -Wconversion does not build.
//!   2. Its PUBLIC compile definitions land on the APP's translation units.
//!      Miss them and the app is preprocessed against a different crypto
//!      configuration than the library it links, with no diagnostic.
//!   3. Its SYSTEM include directories land on the app's TUs as -isystem, so
//!      the vendor's headers do not have to meet the app's -Werror bar.
//!   4. The app links an ARCHIVE, so only referenced members come in.
//!
//! Only (4) announces itself, and only when a symbol is missing entirely.
const std = @import("std");

/// A static library declared in an app's own CMakeLists, built from globbed
/// vendored sources, with usage requirements that reach the app.
pub const VendoredLibrary = struct {
    /// The CMake target name, so `lib<name>.a` matches what the link line of a
    /// real configure names.
    name: []const u8,
    /// Directories globbed NON-recursively for `*.c`, in the order the app's
    /// `file(GLOB ...)` names them. Non-recursive matters: the vendored tree
    /// has subdirectories the glob does not reach.
    source_dirs: []const []const u8,
    /// `target_include_directories(<t> SYSTEM PUBLIC ...)`: on the library's
    /// own TUs and on the app's, both as -isystem.
    system_include_dirs: []const []const u8,
    /// `target_compile_definitions(<t> PUBLIC ...)`: on the library's own TUs
    /// and on the app's. Spelled exactly as the compiler receives them,
    /// embedded quotes included.
    defines: []const []const u8,
    /// `target_compile_options(<t> PRIVATE ...)`: on the library's own TUs
    /// only. Not a diagnostic switch -- it changes code generation, which is
    /// why it survived the audit that deleted the two -Wno- flags beside it.
    compile_options: []const []const u8,
};

/// Everything an app's own CMakeLists adds on top of its ra8_add_app() call.
pub const AppLocal = struct {
    /// `target_compile_definitions(<app>.elf PRIVATE ...)`.
    defines: []const []const u8 = &.{},
    /// `target_include_directories(<app>.elf PRIVATE ...)`, added AFTER
    /// everything ra8_add_app() puts on the path.
    include_dirs: []const []const u8 = &.{},
    /// The vendored static library the app declares and links, if any.
    vendored: ?VendoredLibrary = null,
};

/// The tools and global flag sets a vendored library is built with. The global
/// sets are the toolchain's, not the app target's: that is the whole point.
pub const Toolchain = struct {
    gcc: []const u8,
    ar: []const u8,
    /// CMAKE_C_FLAGS + the build-type flags + the language standard, i.e. what
    /// every TU in this project gets regardless of target.
    global_flags: []const []const u8,
    /// Definitions added at DIRECTORY scope by cmake/toolchain-ra8d2.cmake, so
    /// they reach a target that carries no project profile at all.
    global_defines: []const []const u8,
};

/// The library's globbed translation units, sorted within each directory so
/// the object list is stable across filesystems.
pub fn sources(b: *std.Build, lib: VendoredLibrary) []const []const u8 {
    var out = std.ArrayList([]const u8).init(b.allocator);
    for (lib.source_dirs) |dir_path| {
        var names = std.ArrayList([]const u8).init(b.allocator);
        var dir = b.build_root.handle.openDir(dir_path, .{ .iterate = true }) catch continue;
        defer dir.close();
        var it = dir.iterate();
        while (it.next() catch null) |entry| {
            if (entry.kind != .file) continue;
            if (!std.mem.endsWith(u8, entry.name, ".c")) continue;
            names.append(b.dupe(entry.name)) catch @panic("OOM");
        }
        std.mem.sort([]const u8, names.items, {}, lessThan);
        for (names.items) |name| {
            out.append(b.fmt("{s}/{s}", .{ dir_path, name })) catch @panic("OOM");
        }
    }
    return out.toOwnedSlice() catch @panic("OOM");
}

fn lessThan(_: void, a: []const u8, b_name: []const u8) bool {
    return std.mem.lessThan(u8, a, b_name);
}

/// The flag set one of the library's own TUs is compiled with: the global sets
/// plus its PUBLIC defines plus its PRIVATE options, and deliberately NOT one
/// warning flag. `ra8_target_enable_project_warnings()` is applied by
/// ra8_add_app() to the app target; this library was never handed to it.
pub fn compileFlags(
    allocator: std.mem.Allocator,
    tc: Toolchain,
    lib: VendoredLibrary,
) []const []const u8 {
    var flags = std.ArrayList([]const u8).init(allocator);
    flags.appendSlice(tc.global_flags) catch @panic("OOM");
    flags.appendSlice(tc.global_defines) catch @panic("OOM");
    flags.appendSlice(lib.defines) catch @panic("OOM");
    flags.appendSlice(lib.compile_options) catch @panic("OOM");
    return flags.toOwnedSlice() catch @panic("OOM");
}

/// The defines an app takes from its own CMakeLists: the vendored library's
/// PUBLIC set first, then the app target's own PRIVATE set, which is the order
/// a real configure's database shows.
pub fn appDefines(allocator: std.mem.Allocator, local: AppLocal) []const []const u8 {
    var out = std.ArrayList([]const u8).init(allocator);
    if (local.vendored) |lib| out.appendSlice(lib.defines) catch @panic("OOM");
    out.appendSlice(local.defines) catch @panic("OOM");
    return out.toOwnedSlice() catch @panic("OOM");
}

/// The -isystem directories an app takes from the vendored library it links.
pub fn appSystemIncludeDirs(local: AppLocal) []const []const u8 {
    if (local.vendored) |lib| return lib.system_include_dirs;
    return &.{};
}

/// Compile the library and hand back the archive the app links.
pub fn add(b: *std.Build, lib: VendoredLibrary, tc: Toolchain) std.Build.LazyPath {
    const flags = compileFlags(b.allocator, tc, lib);
    var objects = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (sources(b, lib)) |source| {
        const compile = b.addSystemCommand(&.{tc.gcc});
        compile.addArgs(flags);
        for (lib.system_include_dirs) |include_dir| {
            compile.addArg("-isystem");
            compile.addDirectoryArg(b.path(include_dir));
        }
        compile.addArg("-c");
        compile.addFileArg(b.path(source));
        compile.addArg("-o");
        const object_name = b.fmt("{s}.o", .{std.fs.path.basename(source)});
        objects.append(compile.addOutputFileArg(object_name)) catch @panic("OOM");
    }
    const archive_step = b.addSystemCommand(&.{ tc.ar, "rcs" });
    const archive = archive_step.addOutputFileArg(b.fmt("lib{s}.a", .{lib.name}));
    for (objects.items) |object| archive_step.addFileArg(object);
    return archive;
}

/// Append the library's own compile commands to the database. They are their
/// own rows: same driver and same CPU as the app's, an entirely different bar.
pub fn appendCompileDbEntries(
    b: *std.Build,
    comptime Entry: type,
    out: *std.ArrayList(Entry),
    lib: VendoredLibrary,
    tc: Toolchain,
) void {
    const flags = compileFlags(b.allocator, tc, lib);
    for (sources(b, lib)) |source| {
        out.append(.{
            .file = source,
            .driver = tc.gcc,
            .flags = flags,
            .include_dirs = &.{},
            .system_include_dirs = lib.system_include_dirs,
            .object = b.fmt("arm/{s}/{s}.o", .{ lib.name, std.fs.path.basename(source) }),
        }) catch @panic("OOM");
    }
}
