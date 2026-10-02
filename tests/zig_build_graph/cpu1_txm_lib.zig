//! The module-side ThreadX library for CPU1, the RA8D2's Cortex-M33
//! (RA8FW-413, under RA8FW-292). A module (`.ra8app`) links against this
//! archive rather than the kernel: each `txm_*` call traps through the Module
//! Manager's dispatch, so the module carries no kernel code of its own.
//!
//! The module is loaded wherever the manager finds room, so its code is
//! position independent and its data is reached through r9. Upstream's
//! `ports_module/cortex_m33/gnu/example_build` ships no build script, so the
//! flags come from the sibling GNU module-library scripts
//! (`cortex_m3` / `cortex_m23` `build_threadx_module_library.bat`). Of the
//! two, the M3 form is used: `-mno-pic-data-is-text-relative`, because the
//! M33 manager's `txm_module_manager_thread_stack_build.s` seeds r9 with the
//! module's data start, which is not a fixed offset from its code.
//!
//! The kernel's configuration (tx_user.h and the Module Manager defines) is
//! kept so the objects a module passes across the dispatch agree with the
//! kernel's layout. `zig build txm-m33` builds `lib/libtxm_m33.a`; no image
//! links it, so the ARM images are unchanged.

const std = @import("std");
const middleware = @import("middleware.zig");
const cpu1_threadx = @import("cpu1_threadx.zig");
const modules = @import("cpu1_threadx_modules.zig");

pub const step_description = "Build the ThreadX module library for CPU1 (Cortex-M33) modules as libtxm_m33.a";

/// Position independent code, data through r9, no PLT: what a module the
/// manager relocates at load time needs from every unit it links.
pub const pic_flags = [_][]const u8{
    "-fpic",
    "-fno-plt",
    "-mno-pic-data-is-text-relative",
    "-msingle-pic-base",
};

pub const txm_m33 = middleware.Middleware{
    .name = "txm_m33",
    .soup_c_dirs = &.{
        "pkg:threadx/common_modules/module_lib/src",
        "pkg:threadx/ports_module/cortex_m33/gnu/module_lib/src",
    },
    .soup_asm_dirs = &.{},
    .replaced_basenames = &.{},
    .project_sources = &.{},
    .private_include_dirs = &.{},
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/common_modules/inc",
        "pkg:threadx/ports_module/cortex_m33/gnu/inc",
    },
    .public_defines = modules.threadx_m33_modules.public_defines,
    .link_options = &.{},
};

/// The CPU1 toolchain with the module PIC flags appended to the C line.
pub fn toolchain(allocator: std.mem.Allocator, base: middleware.Toolchain) middleware.Toolchain {
    var tc = cpu1_threadx.toolchain(allocator, base);
    var c_flags = std.ArrayList([]const u8).init(allocator);
    c_flags.appendSlice(tc.c_flags) catch @panic("OOM");
    c_flags.appendSlice(&pic_flags) catch @panic("OOM");
    tc.c_flags = c_flags.toOwnedSlice() catch @panic("OOM");
    return tc;
}

/// Builds the archive and installs it as `lib/libtxm_m33.a` under the
/// prefix, hung off `step`.
pub fn add(b: *std.Build, step: *std.Build.Step, base: middleware.Toolchain) void {
    const archive = middleware.add(b, txm_m33, toolchain(b.allocator, base));
    const install = b.addInstallLibFile(archive, "libtxm_m33.a");
    step.dependOn(&install.step);
}
