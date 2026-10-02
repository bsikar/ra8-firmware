//! The ThreadX Module Manager built for CPU1, the RA8D2's Cortex-M33
//! (RA8FW-415, under RA8FW-292).
//!
//! Upstream ships a module port for cortex_m33 (`ports_module/cortex_m33/gnu`)
//! and none for cortex_m85, so the manager goes to CPU1 first; the M85 side
//! waits on RA8FW-295. This archive is the module-aware kernel, so it stands
//! in for `threadx_m33` rather than sitting on top of it: `common/src` again,
//! the manager's own sources, and the port's `module_manager/src`, whose
//! low-level init, scheduler and context code differ from the plain port's.
//!
//! The port's lowercase `.s` units `#include` and `#define`, so they are
//! listed as `soup_cpp_asm_dirs` and assembled through the preprocessor.
//! None of the port's assembly includes tx_user.h, and the manager's dispatch
//! needs the notify callbacks, which is what the extra defines below are for.
//!
//! A CPU1 image opts in by naming `threadx_m33_modules` in `Cpu1Image.uses`
//! instead of `threadx_m33` (RA8FW-414); naming both is a build error, and
//! either gets the threadx_cpu1 glue. No image in the app table names it yet.
//! `zig build threadx-m33-modules` builds the archive on its own.

const std = @import("std");
const middleware = @import("middleware.zig");
const cpu1_threadx = @import("cpu1_threadx.zig");

pub const step_description = "Build the ThreadX Module Manager for CPU1 (Cortex-M33) as libthreadx_m33_modules.a";

const port_src = "pkg:threadx/ports_module/cortex_m33/gnu/module_manager/src";

pub const threadx_m33_modules = middleware.Middleware{
    .name = "threadx_m33_modules",
    .soup_c_dirs = &.{
        "pkg:threadx/common/src",
        "pkg:threadx/common_modules/module_manager/src",
        port_src,
    },
    .soup_asm_dirs = &.{port_src},
    .soup_cpp_asm_dirs = &.{port_src},
    .replaced_basenames = &.{},
    .project_sources = &.{},
    .private_include_dirs = &.{},
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/common_modules/inc",
        "pkg:threadx/common_modules/module_manager/inc",
        "pkg:threadx/ports_module/cortex_m33/gnu/inc",
    },
    // RA8_THREADX_MODULES keeps the notify callbacks tx_user.h otherwise
    // disables; the Module Manager dispatch needs them. The port's assembly
    // never includes tx_user.h (the plain port's does), so the single-mode
    // switch is passed here, empty, matching tx_user.h's own definition.
    .public_defines = &.{
        "-DTX_INCLUDE_USER_DEFINE_FILE",
        "-DRA8_THREADX_MODULES",
        "-DTX_SINGLE_MODE_SECURE=",
    },
    .link_options = cpu1_threadx.threadx_m33.link_options,
};

/// Builds the archive with the CPU1 toolchain and installs it as
/// `lib/libthreadx_m33_modules.a` under the prefix, hung off `step`.
pub fn add(b: *std.Build, step: *std.Build.Step, base: middleware.Toolchain) void {
    const tc = cpu1_threadx.toolchain(b.allocator, base);
    const archive = middleware.add(b, threadx_m33_modules, tc);
    const install = b.addInstallLibFile(archive, "libthreadx_m33_modules.a");
    step.dependOn(&install.step);
}
