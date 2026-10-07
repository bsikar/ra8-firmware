//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");
const Translator = @import("translate_c").Translator;

/// Turns `includes` into a Zig module. translate-c does not know the C23
/// keywords static_assert and alignof, so they are spelled as the C11
/// keywords they replaced.
fn translateHeaders(
    b: *std.Build,
    target: std.Build.ResolvedTarget,
    optimize: std.lang.OptimizeMode,
    name: []const u8,
    includes: []const u8,
) *std.Build.Module {
    const header = b.addWriteFiles().add(name, includes);
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    translator.defineCMacro("static_assert", "_Static_assert");
    translator.defineCMacro("alignof", "_Alignof");
    translator.addIncludePath(b.path("../inc"));
    translator.addIncludePath(b.path("../src"));
    translator.addIncludePath(b.path("../../../../libs/ra8_core/inc"));
    return translator.mod;
}

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (RA8FW-330).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});
    const adapter = b.createModule(.{
        .root_source_file = b.path("src/adapter.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    const firmware_pipeline_h = translateHeaders(b, target, optimize, "firmware_pipeline_c.h",
        \\#include "firmware_pipeline.h"
        \\#include "firmware_pipeline_rust.h"
        \\
    );
    adapter.addImport("firmware_pipeline_h", firmware_pipeline_h);
    const library = b.addLibrary(.{
        .name = "firmware_pipeline_zig",
        .linkage = .static,
        .root_module = adapter,
    });
    // CMake and cargo link the installed archive with the system linker, which
    // cannot see Zig's compiler_rt. Bundle it so stack-probe helpers such as
    // __zig_probe_stack resolve without the consumer knowing about Zig.
    library.bundle_compiler_rt = true;
    const install_library = b.addInstallArtifact(library, .{});
    b.getInstallStep().dependOn(&install_library.step);
    const library_step = b.step("library", "Build and install the Zig ABI library");
    library_step.dependOn(&install_library.step);

    const supplied_rust_lib_dir = b.option([]const u8, "rust-lib-dir", "Rust archive directory");
    const rust_lib_dir = supplied_rust_lib_dir orelse b.root.joinString(b.allocator, "../rust/target/debug") catch @panic("OOM");
    const rust_archive = b.pathJoin(&.{ rust_lib_dir, "libfirmware_pipeline_rust.a" });

    const executable_module = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    executable_module.addImport("firmware_pipeline_main_h", translateHeaders(b, target, optimize, "firmware_pipeline_main_c.h",
        \\#include "firmware_pipeline.h"
        \\#include "firmware_pipeline_io_internal.h"
        \\
    ));
    // The C sources compiled into the executable still need the headers.
    executable_module.addIncludePath(b.path("../inc"));
    executable_module.addIncludePath(b.path("../src"));
    executable_module.addIncludePath(b.path("../../../../libs/ra8_core/inc"));
    const executable = b.addExecutable(.{
        .name = "firmware_pipeline_zig_main",
        .root_module = executable_module,
    });
    executable.root_module.addObjectFile(library.getEmittedBin());
    executable.root_module.addCSourceFiles(.{
        .files = &.{
            "../src/firmware_pipeline_cli.c",
            "../src/firmware_pipeline_io.c",
        },
        .flags = &.{ "-std=gnu2x", "-Wall", "-Wextra", "-Werror" },
    });
    executable.root_module.addObjectFile(.{ .cwd_relative = rust_archive });
    // `cargo` builds for the machine it runs on, so read the archive before
    // the link and refuse a mismatch by name rather than by linker error
    // (RA8FW-330).
    const require_archive_for_executable = ra8_build.addRequireArchiveForTargetStep(b, executable, rust_archive, "-Drust-lib-dir=");
    executable.step.dependOn(require_archive_for_executable);
    switch (target.result.os.tag) {
        .linux => {
            executable.root_module.linkSystemLibrary("gcc_s", .{});
            executable.root_module.linkSystemLibrary("pthread", .{});
            executable.root_module.linkSystemLibrary("dl", .{});
            executable.root_module.linkSystemLibrary("m", .{});
        },
        .macos => executable.root_module.linkSystemLibrary("System", .{}),
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
    const tests = b.addTest(.{ .root_module = test_module });
    tests.root_module.addObjectFile(.{ .cwd_relative = rust_archive });
    const require_archive_for_tests = ra8_build.addRequireArchiveForTargetStep(b, tests, rust_archive, "-Drust-lib-dir=");
    tests.step.dependOn(require_archive_for_tests);
    switch (target.result.os.tag) {
        .linux => {
            tests.root_module.linkSystemLibrary("gcc_s", .{});
            tests.root_module.linkSystemLibrary("pthread", .{});
            tests.root_module.linkSystemLibrary("dl", .{});
            tests.root_module.linkSystemLibrary("m", .{});
        },
        .macos => tests.root_module.linkSystemLibrary("System", .{}),
        else => @panic("firmware_pipeline Zig tests support Linux and macOS hosts"),
    }
    if (supplied_rust_lib_dir == null) {
        const cargo = b.addSystemCommand(&.{
            "cargo",
            "build",
            "--locked",
            "--lib",
            "--manifest-path",
            b.root.joinString(b.allocator, "../rust/Cargo.toml") catch @panic("OOM"),
        });
        cargo.setEnvironmentVariable("CARGO_TARGET_DIR", b.root.joinString(b.allocator, "../rust/target") catch @panic("OOM"));
        require_archive_for_tests.dependOn(&cargo.step);
        require_archive_for_executable.dependOn(&cargo.step);
    }
    const test_step = b.step("test", "Run native Zig and Zig-to-Rust tests");
    _ = ra8_build.addHostTestRun(b, test_step, tests);
}
