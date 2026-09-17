//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (#899).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
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
    const install_library = b.addInstallArtifact(library, .{});
    b.getInstallStep().dependOn(&install_library.step);
    const library_step = b.step("library", "Build and install the Zig ABI library");
    library_step.dependOn(&install_library.step);

    const supplied_rust_lib_dir = b.option([]const u8, "rust-lib-dir", "Rust archive directory");
    const rust_lib_dir = supplied_rust_lib_dir orelse b.pathFromRoot("../rust/target/debug");

    const executable_module = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    executable_module.addIncludePath(b.path("../inc"));
    executable_module.addIncludePath(b.path("../src"));
    executable_module.addIncludePath(b.path("../../../../libs/ra8_core/inc"));
    const executable = b.addExecutable(.{
        .name = "firmware_pipeline_zig_main",
        .root_module = executable_module,
    });
    executable.addObjectFile(library.getEmittedBin());
    executable.addCSourceFiles(.{
        .files = &.{
            "../src/firmware_pipeline_cli.c",
            "../src/firmware_pipeline_io.c",
        },
        .flags = &.{ "-std=gnu2x", "-Wall", "-Wextra", "-Werror" },
    });
    executable.addObjectFile(.{ .cwd_relative = b.pathJoin(&.{ rust_lib_dir, "libfirmware_pipeline_rust.a" }) });
    switch (target.result.os.tag) {
        .linux => {
            executable.linkSystemLibrary("gcc_s");
            executable.linkSystemLibrary("pthread");
            executable.linkSystemLibrary("dl");
            executable.linkSystemLibrary("m");
        },
        .macos => executable.linkSystemLibrary("System"),
        else => @panic("firmware_pipeline supports Linux and macOS hosts"),
    }
    const install_executable = b.addInstallArtifact(executable, .{});
    b.getInstallStep().dependOn(&install_executable.step);
    const executable_step = b.step("executable", "Build and install the Zig-main executable");
    executable_step.dependOn(&install_executable.step);
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
            "--lib",
            "--manifest-path",
            b.pathFromRoot("../rust/Cargo.toml"),
        });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.pathFromRoot("../rust/target"));
        tests.step.dependOn(&cargo.step);
        executable.step.dependOn(&cargo.step);
    }
    const run_tests = b.addRunArtifact(tests);
    const test_step = b.step("test", "Run native Zig and Zig-to-Rust tests");
    test_step.dependOn(&run_tests.step);
}
