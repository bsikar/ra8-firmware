//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_epd_cal`. CMake consumes
//! the installed static library through the unchanged `inc/ra8_epd_cal.h` C
//! ABI; the `test` step verifies the record codec and the ABI membrane.
//!
//! `-Dbench-vcom-mv=<millivolts>` is the port of the C build's
//! `-DRA8_BENCH_VCOM_MV`, and `-Dproduction-build` is the port of
//! `RA8_PRODUCTION_BUILD`: asking for both is a compile error, exactly as the
//! `#error` in the C translation unit made it.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const bench_vcom_mv = b.option(
        u16,
        "bench-vcom-mv",
        "Bench-only VCOM magnitude in millivolts; never ship a build that sets it",
    );
    const production_build = b.option(
        bool,
        "production-build",
        "Refuse to compile a bench VCOM into the image",
    ) orelse false;

    const build_config = b.addOptions();
    build_config.addOption(?u16, "bench_vcom_mv", bench_vcom_mv);
    build_config.addOption(bool, "production_build", production_build);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_epd_cal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_config", build_config);

    const library = b.addLibrary(.{
        .name = "ra8_epd_cal",
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
        .root_source_file = b.path("src/ra8_epd_cal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_config", build_config);

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

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_epd_cal tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
