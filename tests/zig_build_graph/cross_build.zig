//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The cross toolchain this graph drives, and the context each sub-target is
//! handed: the middleware archive, the app-local vendored library, and the
//! Non-Secure image.
//!
//! Extracted from build.zig by #1179, which had to give every one of these a
//! configuration and put the root build file over the 1000-line ceiling
//! scripts/checks/check_file_size.py holds every Zig source to. The coherent
//! piece is this one: WHO the tools are and WHAT global sets each sub-target
//! gets, as opposed to the step wiring that stays in build.zig.
//!
//! Every function here takes the selected configuration's global sets as a
//! parameter rather than reaching for a build-wide value, so what a middleware
//! TU is given at RelWithDebInfo is an ordinary argument and an ordinary test.

const std = @import("std");

const app_local = @import("app_local.zig");
const arm_flags = @import("arm_flags.zig");
const build_type = @import("build_type.zig");
const middleware = @import("middleware.zig");
const ns_image = @import("ns_image.zig");
const CrossApp = @import("cross_sources.zig").CrossApp;

/// The three cross tools this slice drives.
pub const Tools = struct {
    gcc: []const u8,
    objcopy: []const u8,
    size: []const u8,
    /// The archiver, needed only since #1054: a middleware is handed to the
    /// app as a static archive, and a static link pulls only the members
    /// something references.
    ar: []const u8,
};

pub fn findTools(b: *std.Build) ?Tools {
    const gcc = b.findProgram(&.{"arm-none-eabi-gcc"}, &.{}) catch return null;
    const objcopy = b.findProgram(&.{"arm-none-eabi-objcopy"}, &.{}) catch return null;
    const size = b.findProgram(&.{"arm-none-eabi-size"}, &.{}) catch return null;
    const ar = b.findProgram(&.{"arm-none-eabi-ar"}, &.{}) catch return null;
    return .{ .gcc = gcc, .objcopy = objcopy, .size = size, .ar = ar };
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
        .merge_script = "scripts/gen/merge_ihex.py",
    };
}
