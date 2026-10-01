//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_core` Zig archive, which every image links.
//!
//! `cmake/ra8_app/sources.cmake` registers it unconditionally and says so in
//! the guard it hands to `_ra8_app_require_compilable_lib()`: "links ra8_core
//! into every app". That is not a convenience. `libs/ra8_core/src` holds no
//! `.c` at all since #2820, so the archive is the ONLY place an image can get
//! the freestanding runtime primitives (`memcpy`, `memset`, `str*`, `abs`) the
//! compiler emits calls to from ordinary struct assignment, alongside every
//! other ra8_core port: the log backend, the timebase, the fault block.
//!
//! `libs/ra8_core/build.zig` composes the two host archives into ONE named
//! `ra8_core` when the target is freestanding, because
//! `_ra8_zig_build_archive()` names a cross-built archive `lib<lib>.a` and so
//! can only ever fetch `libra8_core.a`. Asking for `ra8_core` at an ARM target
//! is therefore asking for both roots together, which is what an image wants.

const std = @import("std");

/// The dependency name, the artifact name and the archive basename are all
/// this one string; see the build.zig comment above for why they must be.
pub const lib_name = "ra8_core";

/// An image's own core. `cpu_model` is the consumer's, not the app's: the M33
/// half of a dual-core app links an archive built for cortex_m33 even though
/// the app's main image is M85, because these names have to be present in each
/// image's own instruction set (cmake/ra8_add_app.cmake, the CPU1 expansion).
pub fn forCpu(
    b: *std.Build,
    cpu_model: *const std.Target.Cpu.Model,
    optimize: std.builtin.OptimizeMode,
) std.Build.LazyPath {
    const target = b.resolveTargetQuery(.{
        .cpu_arch = .thumb,
        .os_tag = .freestanding,
        .abi = .eabihf,
        .cpu_model = .{ .explicit = cpu_model },
    });
    return forTarget(b, target, optimize);
}

/// The same archive when the caller already resolved the target.
pub fn forTarget(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) std.Build.LazyPath {
    const dependency = b.dependency(lib_name, .{
        .target = target,
        .optimize = optimize,
    });
    return dependency.artifact(lib_name).getEmittedBin();
}
