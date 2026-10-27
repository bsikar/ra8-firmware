//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_fmt`. The library is header-only today: it owns the
//! format-neutral byte-stream contract (inc/ra8_fmt_io.h) and nothing else,
//! which CMake documents with its intentional no-op. There is no archive to
//! link, so this graph owns two things:
//!
//!   * the include-path install, so a consumer takes inc/ra8_fmt_io.h
//!     from zig-out/include the same way a CMake consumer takes it from
//!     the lib directory;
//!   * a smoke translation unit compiled for the selected target: it
//!     includes the header, pulls in the ra8_err_t contract it depends
//!     on, and static-asserts the four shapes the format engines bind.
//!     The default step runs it, so a cross compile of this library
//!     proves the header itself (RA8FW-976 slice-5 remainder).
//!
//! No options, no sources, no test step: the engines still live under
//! tools/rabook_imagepack until RA8FW-315 promotes them, and there is
//! nothing here to test beyond the smoke TU.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The smoke TU is the whole cross-target story: a freestanding
    // object build either proves the header, or it fails naming the
    // line. It is the default step, which is what the portability
    // gate's --image-builds phase invokes.
    const smoke = b.addObject(.{
        .name = "ra8_fmt_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/ra8_fmt_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    smoke.root_module.addIncludePath(b.path("inc"));
    smoke.root_module.addIncludePath(b.path("../ra8_core/inc"));
    b.default_step.dependOn(&smoke.step);

    // Header-only install: the archive-less consumer story.
    b.default_step.dependOn(&b.addInstallDirectory(.{
        .source_dir = b.path("inc"),
        .install_dir = .header,
        .install_subdir = "",
    }).step);
}
