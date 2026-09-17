//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (#899).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});
    const module = b.createModule(.{
        .root_source_file = b.path("tests/chain_adapter.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    module.addIncludePath(b.path("inc"));
    module.addIncludePath(b.path("../rust_abi_fixture/inc"));
    module.addIncludePath(b.path("../../libs/ra8_core/inc"));
    const library = b.addLibrary(.{ .name = "ra8_abi_chain", .linkage = .static, .root_module = module });
    b.installArtifact(library);

    const supplied_lib_dir = b.option([]const u8, "rust-lib-dir", "Directory containing the Rust ABI archive");
    const rust_lib_dir = supplied_lib_dir orelse b.pathFromRoot("../rust_abi_fixture/target/debug");
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/chain_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addIncludePath(b.path("inc"));
    test_module.addIncludePath(b.path("../rust_abi_fixture/inc"));
    test_module.addIncludePath(b.path("../../libs/ra8_core/inc"));
    const tests = b.addTest(.{ .root_module = test_module });
    tests.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ rust_lib_dir, "libra8_rust_abi_fixture.a" }) });
    switch (target.result.os.tag) {
        .linux => {
            tests.linkSystemLibrary("gcc_s");
            tests.linkSystemLibrary("pthread");
            tests.linkSystemLibrary("dl");
            tests.linkSystemLibrary("m");
        },
        // libSystem carries libc, libm, pthreads and libdl on Darwin.
        .macos => tests.linkSystemLibrary("System"),
        else => @panic("host Zig build graphs support Linux and macOS hosts"),
    }
    if (supplied_lib_dir == null) {
        const cargo = b.addSystemCommand(&.{ "cargo", "build", "--locked", "--manifest-path", b.pathFromRoot("../rust_abi_fixture/Cargo.toml") });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.pathFromRoot("../rust_abi_fixture/target"));
        tests.step.dependOn(&cargo.step);
    }
    const test_step = b.step("test", "Run Zig chain adapter tests");
    const run_tests = b.addRunArtifact(tests);
    test_step.dependOn(&run_tests.step);
    ra8_build.allowForeignHostTests(test_step, tests, run_tests);
}
