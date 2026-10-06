//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The portable interface archive (`libs/if`), linked into every cross image.
//!
//! `libs/if` publishes three C ABIs behind unchanged headers: the filesystem
//! facade (`fw_if_fs.h`), the untrusted-name policy (`ra8_path.h`) and the
//! clock-intent facade (`fw_if_clock.h`). All three are pure dispatch over a
//! caller-supplied vtable, so the archive has no link-time dependency of its
//! own and costs an image nothing it does not reference.
//!
//! It is linked unconditionally rather than swept out of `LIBS`, for the same
//! reason the chip clock adapter is: around twenty-five example `main.c`
//! files call `fw_clock_bind` and `fw_clock_rate_for`, and not one of them
//! names `if` in `LIBS`. `vfs_port_demo` is the only app in the tree that
//! does, and it is not in the cross app table at all, so the derived
//! `LIBS` / `OFF_TARGET_LIBS` sweep cannot reach this archive by
//! construction.

const std = @import("std");
const src_tree = @import("src_tree.zig");

/// The CMake library name, the directory under `libs/`, and the artifact
/// `libs/if/build.zig` installs are all `if`. The build file says why in its
/// own comment: an artifact named anything else "is simply never found".
pub const name = "if";

/// Whether the interface layer is present as a Zig archive to link.
pub fn has(b: *std.Build) bool {
    const build_file = b.fmt("libs/{s}/build.zig", .{name});
    return src_tree.exists(b, build_file);
}

/// The interface archive, built for the consuming image's target.
pub fn forTarget(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
) std.Build.LazyPath {
    const dependency = b.dependency(name, .{
        .target = target,
        .optimize = optimize,
    });
    return dependency.artifact(name).getEmittedBin();
}
