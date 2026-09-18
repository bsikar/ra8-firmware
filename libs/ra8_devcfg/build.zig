//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the `ra8_devcfg` record core.
//! CMake consumes the installed static library through the unchanged
//! `inc/ra8_devcfg.h` C ABI; the `test` step covers the codec and the ABI
//! membrane.
//!
//! One build option, `off-target`: it carries the C's `RA8_OFF_TARGET`
//! preprocessor switch, which chooses whether the production store addresses
//! the extra-MRAM window or the host RAM shadow. It defaults from the target,
//! so a hosted build gets the shadow and a freestanding build gets silicon
//! without CMake passing anything.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The C wrote this as `#ifdef RA8_OFF_TARGET`, set only by the host unit
    // build. A freestanding target is silicon by definition, so derive the
    // default and let an explicit -Doff-target override it.
    const off_target = b.option(
        bool,
        "off-target",
        "Back the default store with a host RAM shadow instead of the extra-MRAM window",
    ) orelse (target.result.os.tag != .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_devcfg_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_devcfg",
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
        .root_source_file = b.path("src/ra8_devcfg_abi.zig"),
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
    abi_module.addOptions("build_config", build_options);
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const store_module = b.createModule(.{
        .root_source_file = b.path("src/store_extra_mram.zig"),
        .target = target,
        .optimize = optimize,
    });
    store_module.addOptions("build_config", build_options);

    const store_test_module = b.createModule(.{
        .root_source_file = b.path("tests/store_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    store_test_module.addImport("store", store_module);
    const store_tests = b.addTest(.{ .root_module = store_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_store_tests = b.addRunArtifact(store_tests);
    const test_step = b.step("test", "Run Zig ra8_devcfg tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_store_tests.step);
}
