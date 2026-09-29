//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the SPI-mode SD card driver
//! core (`ra8_sdmmc_spi`). CMake consumes the installed static library
//! through the unchanged `inc/ra8_sdmmc_spi.h` C ABI and the module-private
//! `src/ra8_sdmmc_spi_internal.h`, which the still-C block-I/O translation
//! unit uses to reach the state object and the helpers exported here.
//!
//! No build options: every byte exchange goes through a caller-supplied
//! transport, so nothing is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_sdmmc_spi_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const library = b.addLibrary(.{
        .name = "ra8_sdmmc_spi",
        .linkage = .static,
        .root_module = library_module,
    });
    // CMake links this archive with the system linker, so it must carry the
    // Zig runtime helpers itself.  The host ABI fixtures also consume it from
    // PIE executables.
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_sdmmc_spi_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
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

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_sdmmc_spi tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
