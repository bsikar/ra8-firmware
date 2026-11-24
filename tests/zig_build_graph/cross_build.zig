//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The cross toolchain this graph drives, and the context each sub-target is
//! handed: the middleware archive, the app-local vendored library, and the
//! Non-Secure image.
//!
//! Extracted from build.zig by the build-type slice, which had to give every one of these a
//! configuration and put the root build file over the 1000-line ceiling
//! scripts/checks/check_file_size.py holds every Zig source to. The coherent
//! piece is this one: WHO the tools are and WHAT global sets each sub-target
//! gets, as opposed to the step wiring that stays in build.zig.
//!
//! Every function here takes the selected configuration's global sets as a
//! parameter rather than reaching for a build-wide value, so what a middleware
//! TU is given at RelWithDebInfo is an ordinary argument and an ordinary test.

const builtin = @import("builtin");
const std = @import("std");

const app_local = @import("app_local.zig");
const arm_flags = @import("arm_flags.zig");
const build_type = @import("build_type.zig");
const middleware = @import("middleware.zig");
const ns_image = @import("ns_image.zig");
const cross_sources = @import("cross_sources.zig");
const CrossApp = cross_sources.CrossApp;

/// The three cross tools this slice drives.
pub const Tools = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
    /// The archiver, needed only since middleware support: a middleware is handed to the
    /// app as a static archive, and a static link pulls only the members
    /// something references.
    ar: []const u8,
};

pub fn findTools(b: *std.Build) ?Tools {
    const gcc = findProgram(b, "arm-none-eabi-gcc") orelse return null;
    const objcopy = findProgram(b, "arm-none-eabi-objcopy") orelse return null;
    const size = findProgram(b, "arm-none-eabi-size") orelse return null;
    const ar = findProgram(b, "arm-none-eabi-ar") orelse return null;
    return .{ .gcc = gcc, .objcopy = objcopy, .size = size, .ar = ar };
}

pub fn findProgram(b: *std.Build, name: []const u8) ?[]const u8 {
    if (builtin.os.tag != .windows) {
        return b.findProgram(.{ .names = &.{name} });
    }
    return findProgramWindows(b, name);
}

/// Validate and unquote one Windows `PATH` directory entry before passing it
/// to `std.Io.Dir`. Zig 0.17's `std.Build.findProgram` joins raw `PATH`
/// segments and calls `Io.Dir.cwd().access`, whose Windows implementation
/// panics on `0xc0000033 (OBJECT_NAME_INVALID)` when `PATH` contains
/// malformed entries such as concatenated paths with multiple drive colons or
/// quoted segments.
pub fn sanitizeWindowsPathEntry(raw: []const u8) ?[]const u8 {
    const unquoted = if (raw.len >= 2 and raw[0] == '"' and raw[raw.len - 1] == '"')
        raw[1 .. raw.len - 1]
    else
        raw;
    if (unquoted.len == 0 or unquoted.len >= 32767) return null;

    for (unquoted, 0..) |c, i| {
        if (c < 0x20) return null;
        switch (c) {
            '"', '<', '>', '|', '*' => return null,
            '?' => {
                // Allow `?` only at index 2 of a `\\?\` or `//?/` Win32 prefix.
                if (i != 2 or unquoted.len < 4 or
                    !isSlash(unquoted[0]) or !isSlash(unquoted[1]) or !isSlash(unquoted[3]))
                {
                    return null;
                }
            },
            else => {},
        }
    }

    // Reject bare `\\`, `\\?`, and `\\.` prefixes that have no path body.
    if (unquoted.len == 2 and isSlash(unquoted[0]) and isSlash(unquoted[1])) return null;
    if (unquoted.len == 3 and isSlash(unquoted[0]) and isSlash(unquoted[1]) and
        (unquoted[2] == '?' or unquoted[2] == '.'))
    {
        return null;
    }

    const prefix_len: usize = if (unquoted.len >= 4 and isSlash(unquoted[0]) and isSlash(unquoted[1]) and
        (unquoted[2] == '?' or unquoted[2] == '.') and isSlash(unquoted[3]))
        4
    else
        0;
    const rest = unquoted[prefix_len..];

    // Colons are valid only immediately after a single leading drive letter.
    const drive_end: usize = if (prefix_len == 4 and rest.len >= 4 and
        std.ascii.eqlIgnoreCase(rest[0..3], "UNC") and isSlash(rest[3]))
        0
    else if (rest.len >= 2 and std.ascii.isAlphabetic(rest[0]) and rest[1] == ':')
        2
    else
        0;
    if (std.mem.indexOfScalar(u8, rest[drive_end..], ':') != null) return null;

    return unquoted;
}

fn isSlash(c: u8) bool {
    return c == '/' or c == '\\';
}

fn isSupportedWindowsExt(ext: []const u8) bool {
    inline for (@typeInfo(std.process.WindowsExtension).@"enum".field_names) |field_name| {
        if (std.ascii.eqlIgnoreCase(ext, "." ++ field_name)) return true;
    }
    return false;
}

/// Probe one directory for `name` or `name` plus a supported Windows extension.
pub fn findExecutableInDir(
    allocator: std.mem.Allocator,
    io: std.Io,
    dir_path: []const u8,
    name: []const u8,
    pathext: ?[]const u8,
) ?[]const u8 {
    var dir = std.Io.Dir.cwd().openDir(io, dir_path, .{}) catch return null;
    defer dir.close(io);

    if (dir.statFile(io, name, .{})) |stat| {
        if (stat.kind != .directory) {
            return std.fs.path.join(allocator, &.{ dir_path, name }) catch @panic("OOM");
        }
    } else |_| {}

    const raw_pathext = pathext orelse ".COM;.EXE;.BAT;.CMD";
    var ext_it = std.mem.tokenizeScalar(u8, raw_pathext, ';');
    var buf: [std.fs.max_name_bytes]u8 = undefined;
    while (ext_it.next()) |ext| {
        if (!isSupportedWindowsExt(ext)) continue;
        const extended = std.fmt.bufPrint(&buf, "{s}{s}", .{ name, ext }) catch continue;
        if (dir.statFile(io, extended, .{})) |stat| {
            if (stat.kind != .directory) {
                return std.fs.path.join(allocator, &.{ dir_path, extended }) catch @panic("OOM");
            }
        } else |_| {}
    }
    return null;
}

/// Search `path_env` entries separated by `delimiter`, skipping malformed entries.
pub fn findExecutableInPath(
    allocator: std.mem.Allocator,
    io: std.Io,
    path_env: []const u8,
    delimiter: u8,
    name: []const u8,
    pathext: ?[]const u8,
) ?[]const u8 {
    var it = std.mem.tokenizeScalar(u8, path_env, delimiter);
    while (it.next()) |raw_dir| {
        const clean_dir = sanitizeWindowsPathEntry(raw_dir) orelse continue;
        if (findExecutableInDir(allocator, io, clean_dir, name, pathext)) |found| return found;
    }
    return null;
}

fn findProgramWindows(b: *std.Build, name: []const u8) ?[]const u8 {
    b.graph.poisonCache();
    const io = b.graph.io;
    const pathext = b.graph.environ_map.get("PATHEXT");

    for (b.graph.search_prefixes.items) |search_prefix| {
        const bin_dir = b.pathJoin(&.{ search_prefix, "bin" });
        if (sanitizeWindowsPathEntry(bin_dir)) |clean_dir| {
            if (findExecutableInDir(b.allocator, io, clean_dir, name, pathext)) |found| return found;
        }
    }

    if (b.graph.environ_map.get("PATH")) |path_env| {
        return findExecutableInPath(b.allocator, io, path_env, ';', name, pathext);
    }

    return null;
}

/// The two global flag sets a middleware archive is built with, and the tools
/// that build it. Named once so `zig build arm` and `zig build compile-db`
/// cannot drift apart about what a middleware TU is really given.
/// The same two global sets, handed to an app-local vendored library. It gets
/// the toolchain's flags and the directory-scope defines, and none of the
/// project warning profile: that profile is applied by ra8_add_app() to the
/// app target, and a separately-declared library never passed through it.
pub fn appLocalToolchain(tools: Tools, arm: build_type.Globals, global_defines: []const []const u8) app_local.Toolchain {
    return .{
        .gcc = tools.gcc,
        .ar = tools.ar,
        .global_flags = arm.c_flags,
        .global_defines = global_defines,
    };
}

pub fn middlewareToolchain(tools: Tools, arm: build_type.Globals, global_defines: []const []const u8) middleware.Toolchain {
    return .{
        .gcc = tools.gcc,
        .ar = tools.ar,
        .global_defines = global_defines,
        .c_flags = arm.c_flags,
        .asm_flags = arm.asm_flags,
    };
}

/// The Non-Secure image's own context: the tools, the app it belongs to, the
/// two global flag sets, and the warning profile at ITS frame budget rather
/// than the app's. Named once so the `arm` step and the compile database
/// cannot drift apart about what an NS translation unit is really given.
pub fn nsContext(
    b: *std.Build,
    tools: Tools,
    app: CrossApp,
    image: ns_image.NsImage,
    arm: build_type.Globals,
    global_defines: []const []const u8,
) ns_image.Context {
    const mw = middleware.find(image.uses) orelse std.debug.panic(
        "ra8: the NS image names middleware {s}, which the root build graph does not know yet",
        .{image.uses},
    );
    return .{
        .gcc = tools.gcc,
        .objcopy = tools.objcopy,
        .size = tools.size,
        .app_name = app.name,
        .app_dir = app.dir,
        .image = image,
        .middleware = mw,
        .global_defines = global_defines,
        .global_compile_flags = arm.c_flags,
        .warning_flags = arm_flags.warningFlagsForStack(b.allocator, image.stack_bytes),
        .global_link_flags = arm.link_flags,
        .zig_target = b.resolveTargetQuery(cross_sources.zig_target_query),
        .zig_optimize = arm.configuration.zig_optimize,
    };
}
