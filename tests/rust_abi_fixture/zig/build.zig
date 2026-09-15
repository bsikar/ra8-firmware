//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build the private Zig consumer against the same Rust archive used by C.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const supplied_lib_dir = b.option([]const u8, "rust-lib-dir", "Directory containing the Rust ABI archive");
    const rust_lib_dir = supplied_lib_dir orelse b.pathFromRoot("../target/debug");

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/consumer_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.link_libc = true;
    test_module.addIncludePath(b.path("../inc"));
    test_module.addIncludePath(b.path("../../../libs/ra8_core/inc"));

    const tests = b.addTest(.{ .root_module = test_module });
    tests.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ rust_lib_dir, "libra8_rust_abi_fixture.a" }) });
    tests.linkSystemLibrary("gcc_s");
    tests.linkSystemLibrary("pthread");
    tests.linkSystemLibrary("dl");
    tests.linkSystemLibrary("m");

    if (supplied_lib_dir == null) {
        const cargo = b.addSystemCommand(&.{ "cargo", "build", "--locked", "--manifest-path", b.pathFromRoot("../Cargo.toml") });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.pathFromRoot("../target"));
        tests.step.dependOn(&cargo.step);
    }

    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run Zig consumer of the Rust C ABI fixture");
    test_step.dependOn(&run_tests.step);
}
