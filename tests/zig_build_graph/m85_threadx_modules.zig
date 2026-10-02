//! The ThreadX Module Manager built for the M85 (RA8FW-426, under RA8FW-295).
//!
//! Upstream ships no `ports_module/cortex_m85`, but its plain cortex_m85/gnu
//! port is the cortex_m33/gnu port with a different version string: every
//! source file and tx_port.h match once comments are stripped. So the M85
//! Module Manager is upstream's `ports_module/cortex_m33/gnu` built with the
//! M85 middleware toolchain, and no vendored file is forked. Whether that
//! scheduler keeps Helium state (RA8FW-428) and programs the PMSAv8.1 MPU
//! right (RA8FW-427) are their own tickets.
//!
//! Everything else is the M85 kernel's own configuration (`middleware.threadx`):
//! the project-tuned low-level init replaces upstream's, the SysTick glue
//! comes along, and the Module Manager defines match the CPU1 archive's.
//! `zig build threadx-m85-modules` builds `lib/libthreadx_m85_modules.a`; no
//! image links it, so every ARM image is unchanged.

const std = @import("std");
const middleware = @import("middleware.zig");
const modules = @import("cpu1_threadx_modules.zig");

pub const step_description = "Build the ThreadX Module Manager for the M85 as libthreadx_m85_modules.a";

const port_src = "pkg:threadx/ports_module/cortex_m33/gnu/module_manager/src";
const kernel = middleware.threadx;

pub const threadx_m85_modules = middleware.Middleware{
    .name = "threadx_m85_modules",
    .soup_c_dirs = &.{
        "pkg:threadx/common/src",
        "pkg:threadx/common_modules/module_manager/src",
        port_src,
    },
    .soup_asm_dirs = &.{port_src},
    .soup_cpp_asm_dirs = &.{port_src},
    .replaced_basenames = kernel.replaced_basenames,
    .project_sources = kernel.project_sources,
    .private_include_dirs = kernel.private_include_dirs,
    .public_include_dirs = kernel.public_include_dirs,
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/common_modules/inc",
        "pkg:threadx/common_modules/module_manager/inc",
        "pkg:threadx/ports_module/cortex_m33/gnu/inc",
    },
    .public_defines = modules.threadx_m33_modules.public_defines,
    .link_options = kernel.link_options,
};

/// Builds the archive with the M85 middleware toolchain and installs it as
/// `lib/libthreadx_m85_modules.a` under the prefix, hung off `step`.
pub fn add(b: *std.Build, step: *std.Build.Step, tc: middleware.Toolchain) void {
    const archive = middleware.add(b, threadx_m85_modules, tc);
    const install = b.addInstallLibFile(archive, "libthreadx_m85_modules.a");
    step.dependOn(&install.step);
}
