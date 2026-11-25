//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host parity and execution harness for the shared `ra8_box` ABI vectors:
//! verifies both the C23 runner executable and 1:1 equivalence between
//! `vectors.json` and `inc/ra8_box_pair_vectors.h`.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const box_dep = b.dependency("ra8_box", .{
        .target = target,
        .optimize = optimize,
    });
    const box_lib = box_dep.artifact("ra8_box");
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core_zig");
    const ui_lib = b.dependency("ra8_ui", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_ui");

    const host_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    host_module.addCSourceFiles(.{
        .files = &.{
            "src/ra8_box_pair_runner.c",
            "src/host_log_stub.c",
            "src/host_vector_verify.c",
        },
        .flags = &.{"-std=c23"},
    });
    host_module.addIncludePath(b.path("inc"));
    host_module.addIncludePath(box_lib.getEmittedIncludeTree());
    host_module.addIncludePath(ui_lib.getEmittedIncludeTree());
    host_module.addIncludePath(core_lib.getEmittedIncludeTree());
    host_module.linkLibrary(box_lib);

    const host_verify = b.addExecutable(.{
        .name = "host_vector_verify",
        .root_module = host_module,
    });

    const run_c_vectors = b.addRunArtifact(host_verify);

    const dump_c_json = b.addRunArtifact(host_verify);
    dump_c_json.addArg("--dump-json");
    const c_json_path = dump_c_json.captureStdOut(.{});

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/verify_vectors_test.zig"),
        .target = target,
        .optimize = optimize,
        .link_libc = true,
    });
    test_module.addAnonymousImport("vectors_json", .{
        .root_source_file = b.path("vectors.json"),
    });
    test_module.addAnonymousImport("c_vectors_json", .{
        .root_source_file = c_json_path,
    });
    test_module.addCSourceFiles(.{
        .files = &.{"src/host_log_stub.c"},
        .flags = &.{"-std=c23"},
    });
    test_module.addIncludePath(core_lib.getEmittedIncludeTree());
    test_module.linkLibrary(box_lib);

    const zig_verify_tests = b.addTest(.{ .root_module = test_module });
    const run_zig_vectors = b.addRunArtifact(zig_verify_tests);

    const test_step = b.step("test", "Verify C and JSON behavior vectors against libra8_box.a");
    test_step.dependOn(&run_c_vectors.step);
    test_step.dependOn(&run_zig_vectors.step);
}
