//! The ThreadX kernel built for CPU1, the RA8D2's Cortex-M33 (RA8FW-396).
//!
//! `middleware.threadx` is the kernel for the M85: its port directory is
//! `ports/cortex_m85/gnu` and its toolchain is the app's, so its objects are
//! M85 objects. The M33 half of a dual-core app cannot link them. This file is
//! the same kernel from `ports/cortex_m33/gnu`, compiled with the CPU1 flags
//! that `cpu1_image.compileFlags()` already gives every CPU1 translation unit.
//!
//! What it keeps from the M85 entry: `common/src`, the project
//! `port/threadx/inc/tx_user.h` (TX_SINGLE_MODE_SECURE, 1 kHz tick) and the
//! define that makes the kernel read it. What it drops: the three project
//! sources under `port/threadx/src/cortex_m85`. The low-level init there is
//! M85-tuned and the SysTick retune reads the CGC through ra8_hal, which the
//! CPU1 image does not link. The upstream `tx_initialize_low_level.S` stays.
//!
//! No app links this archive yet. `zig build threadx-m33` builds it so the
//! port is proven to compile at -mcpu=cortex-m33 before an example uses it.

const std = @import("std");
const middleware = @import("middleware.zig");
const cpu1_image = @import("cpu1_image.zig");

pub const step_description = "Build the ThreadX kernel for CPU1 (Cortex-M33) as libthreadx_m33.a";

pub const threadx_m33 = middleware.Middleware{
    .name = "threadx_m33",
    .soup_c_dirs = &.{
        "pkg:threadx/common/src",
        "pkg:threadx/ports/cortex_m33/gnu/src",
    },
    .soup_asm_dirs = &.{"pkg:threadx/ports/cortex_m33/gnu/src"},
    .replaced_basenames = &.{},
    .project_sources = &.{},
    .private_include_dirs = &.{},
    .public_include_dirs = &.{"port/threadx/inc"},
    .public_system_include_dirs = &.{
        "pkg:threadx/common/inc",
        "pkg:threadx/ports/cortex_m33/gnu/inc",
    },
    .public_defines = &.{"-DTX_INCLUDE_USER_DEFINE_FILE"},
    .link_options = &.{"-Wl,--undefined=_tx_timer_interrupt"},
};

/// The M85 middleware toolchain turned into a CPU1 one. C takes
/// `cpu1_image.compileFlags()`, so the CPU1 defines come first and the M33
/// target flags last (gcc keeps the last -mcpu). Assembly takes the global
/// assembly flags, then the CPU1 defines and target flags, in the same order.
pub fn toolchain(allocator: std.mem.Allocator, base: middleware.Toolchain) middleware.Toolchain {
    var asm_flags = std.ArrayList([]const u8).init(allocator);
    asm_flags.appendSlice(&cpu1_image.defines) catch @panic("OOM");
    asm_flags.appendSlice(base.asm_flags) catch @panic("OOM");
    asm_flags.appendSlice(&cpu1_image.target_flags) catch @panic("OOM");
    return .{
        .gcc = base.gcc,
        .ar = base.ar,
        .global_defines = base.global_defines,
        .c_flags = cpu1_image.compileFlags(allocator, base.c_flags),
        .asm_flags = asm_flags.toOwnedSlice() catch @panic("OOM"),
    };
}

/// Builds the archive and installs it as `lib/libthreadx_m33.a` under the
/// prefix, hung off `step`.
pub fn add(b: *std.Build, step: *std.Build.Step, base: middleware.Toolchain) void {
    const archive = middleware.add(b, threadx_m33, toolchain(b.allocator, base));
    const install = b.addInstallLibFile(archive, "libthreadx_m33.a");
    step.dependOn(&install.step);
}

/// The middleware a CPU1 image may name in `Cpu1Image.uses`. Kept apart from
/// `middleware.find()` on purpose: an M85 app naming `threadx_m33` would link
/// M33 objects into an M85 image, and that has to stay unrepresentable.
const known = [_]middleware.Middleware{threadx_m33};

pub fn find(name: []const u8) ?middleware.Middleware {
    for (known) |candidate| {
        if (std.mem.eql(u8, candidate.name, name)) return candidate;
    }
    return null;
}

/// Every middleware a CPU1 image names, in order. An unknown name is a build
/// error, the same rule `middleware.resolve()` applies to M85 apps.
pub fn resolve(allocator: std.mem.Allocator, uses: []const []const u8) []const middleware.Middleware {
    var out = std.ArrayList(middleware.Middleware).init(allocator);
    for (uses) |name| {
        const record = find(name) orelse std.debug.panic(
            "ra8: a CPU1 image names USES {s}, which the CPU1 graph does not know",
            .{name},
        );
        out.append(record) catch @panic("OOM");
    }
    return out.toOwnedSlice() catch @panic("OOM");
}

/// One archive per named middleware, built with the CPU1 toolchain.
pub fn archives(b: *std.Build, uses: []const []const u8, base: middleware.Toolchain) []const std.Build.LazyPath {
    const tc = toolchain(b.allocator, base);
    var out = std.ArrayList(std.Build.LazyPath).init(b.allocator);
    for (resolve(b.allocator, uses)) |mw| out.append(middleware.add(b, mw, tc)) catch @panic("OOM");
    return out.items;
}
