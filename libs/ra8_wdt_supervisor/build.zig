//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_wdt_supervisor`. CMake
//! consumes the installed static library through the unchanged
//! `inc/ra8_wdt_supervisor.h` C ABI; the `test` step covers the policy core and
//! the ABI membrane.
//!
//! One build option, `off-target`: it carries the C's `RA8_OFF_TARGET` switch,
//! which chose between the real ThreadX API and the host stand-ins. It defaults
//! from the target, so a hosted build gets the stand-ins and a freestanding
//! build calls ThreadX without CMake passing anything.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Use the host ThreadX stand-ins instead of calling the real kernel",
    ) orelse (target.result.os.tag != .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wdt_supervisor_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_wdt_supervisor",
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

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_wdt_supervisor_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_config", build_options);

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_wdt_supervisor tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
