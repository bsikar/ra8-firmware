//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const adapter = b.createModule(.{
        .root_source_file = b.path("src/adapter.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    adapter.addIncludePath(b.path("../inc"));
    const library = b.addLibrary(.{
        .name = "firmware_pipeline_zig",
        .linkage = .static,
        .root_module = adapter,
    });
    b.installArtifact(library);

    const supplied_rust_lib_dir = b.option([]const u8, "rust-lib-dir", "Rust archive directory");
    const rust_lib_dir = supplied_rust_lib_dir orelse b.pathFromRoot("../rust/target/debug");
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/native.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addImport("adapter", adapter);
    test_module.addIncludePath(b.path("../inc"));
    const tests = b.addTest(.{ .root_module = test_module });
    tests.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ rust_lib_dir, "libfirmware_pipeline_rust.a" }) });
    switch (target.result.os.tag) {
        .linux => {
            tests.linkSystemLibrary("gcc_s");
            tests.linkSystemLibrary("pthread");
            tests.linkSystemLibrary("dl");
            tests.linkSystemLibrary("m");
        },
        .macos => tests.linkSystemLibrary("System"),
        else => @panic("firmware_pipeline Zig tests support Linux and macOS hosts"),
    }
    if (supplied_rust_lib_dir == null) {
        const cargo = b.addSystemCommand(&.{
            "cargo",
            "build",
            "--locked",
            "--manifest-path",
            b.pathFromRoot("../rust/Cargo.toml"),
        });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.pathFromRoot("../rust/target"));
        tests.step.dependOn(&cargo.step);
    }
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run native Zig and Zig-to-Rust tests");
    test_step.dependOn(&run_tests.step);
}
