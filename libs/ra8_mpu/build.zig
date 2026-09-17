//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_mpu`. CMake consumes the
//! installed static library through the unchanged `inc/ra8_mpu.h` C ABI; the
//! `test` step covers the encoders and the ABI membrane.
//!
//! One build option, `off-target`: it carries the C's `RA8_OFF_TARGET`
//! switch, which decides whether the DSB/ISB barriers come from the host stub
//! translation unit or are emitted inline. It defaults from the target, so a
//! hosted build gets the stub and a freestanding build gets the instructions
//! without CMake passing anything.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Call the host barrier stubs instead of emitting DSB/ISB inline",
    ) orelse (target.result.os.tag != .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_mpu_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_mpu",
        .linkage = .static,
        .root_module = library_module,
    });
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_mpu_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_config", build_options);

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
    const test_step = b.step("test", "Run Zig ra8_mpu tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
