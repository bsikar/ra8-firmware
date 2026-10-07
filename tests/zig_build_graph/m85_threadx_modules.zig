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
//! The one vendored value it changes is the module MPU budget (RA8FW-484),
//! through a generated copy of txm_module_port.h rather than a fork.
//! `zig build threadx-m85-modules` builds `lib/libthreadx_m85_modules.a`; no
//! image links it, so every ARM image is unchanged.

const std = @import("std");
const middleware = @import("middleware.zig");
const modules = @import("cpu1_threadx_modules.zig");

pub const step_description = "Build the ThreadX Module Manager for the M85 as libthreadx_m85_modules.a";

const port_src = "pkg:threadx/ports_module/cortex_m33/gnu/module_manager/src";
const kernel = middleware.threadx;

/// The M85 scheduler that keeps the board MPU map for kernel threads
/// (RA8FW-481), in place of the module port's own.
pub const schedule_source = "port/threadx/src/cortex_m85_modules/tx_thread_schedule.S";
/// The shared-memory grant that refuses the board's shared SRAM (RA8FW-527),
/// in Zig, in place of the module port's C.
pub const external_memory_source = "port/threadx/src/cortex_m85_modules/external_memory_enable.zig";
const replaced_basenames = kernel.replaced_basenames[0..kernel.replaced_basenames.len].* ++ [_][]const u8{
    "tx_thread_schedule.S",
    "txm_module_manager_external_memory_enable.c",
};
const project_sources = kernel.project_sources[0..kernel.project_sources.len].* ++ [_][]const u8{schedule_source};

/// The M85 MPU budget (RA8FW-484). DREGION is 8 on the M85. A module keeps
/// 7 of them (kernel entry, code, data and 4 shared grants), and the last one
/// stays a privileged-only, non-cacheable copy of the board's shared-SRAM
/// region (board slot 4), so an ISR that runs while a module thread is
/// current still sees the M85 to M33 mailbox as coherent.
pub const dregion = 8;
pub const module_entries = 7;
pub const shared_entries = 4;
pub const board_entries = dregion - module_entries;
pub const shared_board_region = 4;

/// The vendored header hard-codes 8 and 5 without an `#ifndef`, so the
/// archive compiles a generated copy with those two lines rewritten.
pub const port_header = middleware.HeaderPatch{
    .header = "pkg:threadx/ports_module/cortex_m33/gnu/inc/txm_module_port.h",
    .rewrites = &.{
        .{
            .old = "#define TXM_MODULE_MPU_TOTAL_ENTRIES            8",
            .new = std.fmt.comptimePrint("#define TXM_MODULE_MPU_TOTAL_ENTRIES            {d}", .{module_entries}),
        },
        .{
            .old = "#define TXM_MODULE_MPU_SHARED_ENTRIES           5",
            .new = std.fmt.comptimePrint("#define TXM_MODULE_MPU_SHARED_ENTRIES           {d}", .{shared_entries}),
        },
    },
};

pub const threadx_m85_modules = middleware.Middleware{
    .name = "threadx_m85_modules",
    .soup_c_dirs = &.{
        "pkg:threadx/common/src",
        "pkg:threadx/common_modules/module_manager/src",
        port_src,
    },
    .soup_asm_dirs = &.{port_src},
    .soup_cpp_asm_dirs = &.{port_src},
    .replaced_basenames = &replaced_basenames,
    .project_sources = &project_sources,
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
    .patched_headers = &.{port_header},
    .zig_sources = &.{.{
        .path = external_memory_source,
        .cpu = &std.Target.arm.cpu.cortex_m85,
        .c_headers = &.{"txm_module.h"},
    }},
};

/// Builds the archive with the M85 middleware toolchain and installs it as
/// `lib/libthreadx_m85_modules.a` under the prefix, hung off `step`.
pub fn add(b: *std.Build, step: *std.Build.Step, tc: middleware.Toolchain) void {
    const archive = middleware.add(b, threadx_m85_modules, tc);
    const install = b.addInstallLibFile(archive, "libthreadx_m85_modules.a");
    step.dependOn(&install.step);
}
