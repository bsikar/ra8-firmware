//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_jpeg`. The codec itself is still C: the marker walk,
//! the whole-buffer decoder, the encoder and the streaming driver share
//! `src/ra8_jpeg_sw_internal.h` and are ported in later slices. This archive
//! carries the `ra8_imgdec` backend (`inc/ra8_jpeg_imgdec.h`), which reaches
//! the codec only through its public header, so it was portable on its own.
//!
//! The `test` step drives the backend's decisions: the `dim_max` ceiling and
//! the destination-fit verdicts, neither of which needs a JPEG.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library = b.addLibrary(.{
        .name = "ra8_jpeg",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/jpeg_imgdec_abi.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    // The host C test executables are linked by the system toolchain, not by
    // `zig cc`, so nothing else on that link line provides Zig's runtime
    // helpers. Without this the archive leaves `__zig_probe_stack` undefined.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const policy_module = b.createModule(.{
        .root_source_file = b.path("src/internal/imgdec.zig"),
        .target = target,
        .optimize = optimize,
    });

    const imgdec_test_module = b.createModule(.{
        .root_source_file = b.path("tests/imgdec_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    imgdec_test_module.addImport("policy", policy_module);
    const imgdec_tests = b.addTest(.{ .root_module = imgdec_test_module });

    const run_imgdec_tests = b.addRunArtifact(imgdec_tests);
    const test_step = b.step("test", "Run Zig ra8_jpeg tests");
    test_step.dependOn(&run_imgdec_tests.step);
}
