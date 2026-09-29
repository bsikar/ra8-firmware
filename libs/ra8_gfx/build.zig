//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_gfx` rasteriser. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_gfx.h`, `inc/ra8_gfx_tone.h`, `inc/ra8_gfx_dither.h` and
//! `src/ra8_gfx_internal.h`. The only C translation unit left in the library
//! is the generated font table, which links against this archive for
//! `g_gfx_text_state` and the promoted helpers.
//!
//! No build options: nothing in this cluster is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_gfx_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_gfx",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_gfx_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const tone_module = b.createModule(.{
        .root_source_file = b.path("src/internal/tone.zig"),
        .target = target,
        .optimize = optimize,
    });

    const tone_test_module = b.createModule(.{
        .root_source_file = b.path("tests/tone_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    tone_test_module.addImport("tone", tone_module);
    const tone_tests = b.addTest(.{ .root_module = tone_test_module });

    const dither_module = b.createModule(.{
        .root_source_file = b.path("src/internal/dither.zig"),
        .target = target,
        .optimize = optimize,
    });

    const dither_test_module = b.createModule(.{
        .root_source_file = b.path("tests/dither_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    dither_test_module.addImport("dither", dither_module);
    const dither_tests = b.addTest(.{ .root_module = dither_test_module });

    const bind_module = b.createModule(.{
        .root_source_file = b.path("src/internal/bind.zig"),
        .target = target,
        .optimize = optimize,
    });

    const bind_test_module = b.createModule(.{
        .root_source_file = b.path("tests/bind_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    bind_test_module.addImport("bind", bind_module);
    const bind_tests = b.addTest(.{ .root_module = bind_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_tone_tests = b.addRunArtifact(tone_tests);
    const run_dither_tests = b.addRunArtifact(dither_tests);
    const run_bind_tests = b.addRunArtifact(bind_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_gfx tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_tone_tests.step);
    test_step.dependOn(&run_dither_tests.step);
    test_step.dependOn(&run_bind_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
