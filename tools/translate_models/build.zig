//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host ABI golden test build graph for `tools/translate_models`.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const translate_dep = b.dependency("ra8_translate", .{
        .target = target,
        .optimize = optimize,
    });
    const translate_lib = translate_dep.artifact("ra8_translate");

    const goldens_mod = b.createModule(.{
        .root_source_file = b.path("generated/translate_model_goldens.zig"),
        .target = target,
        .optimize = optimize,
    });

    const test_mod = b.createModule(.{
        .root_source_file = b.path("tests/golden_abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_mod.addImport("goldens", goldens_mod);
    test_mod.linkLibrary(translate_lib);

    const abi_tests = b.addTest(.{ .root_module = test_mod });
    const test_step = b.step("test", "Verify real-model goldens against libs/ra8_translate C ABI");
    const run_abi_tests = b.addRunArtifact(abi_tests);
    ra8_build.configureHostTestRun(b, test_step, abi_tests, run_abi_tests);
    test_step.dependOn(&run_abi_tests.step);
}
