//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

fn addCodec(module: *std.Build.Module, b: *std.Build) void {
    module.addIncludePath(b.path("../../../libs/ra8_jpeg/inc"));
    module.addIncludePath(b.path("../../../libs/ra8_jpeg/src"));
    module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    module.addCSourceFiles(.{
        .files = &.{
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_stream.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_encode.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_encode_emit.c",
            "../../../libs/ra8_core/src/ra8_log.c",
            "../../../libs/ra8_core/src/ra8_error_handler.c",
        },
        .flags = &.{
            "-std=gnu2x",
            "-DRA8_FREESTANDING",
            "-DRA8_OFF_TARGET",
            "-Wall",
            "-Wextra",
            "-Werror",
        },
    });
    module.link_libc = true;
}

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (#899).
    const target = b.standardTargetOptions(.{ .default_target = ra8_build.hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});
    const app_module = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    addCodec(app_module, b);

    const executable = b.addExecutable(.{ .name = "image_pyramid", .root_module = app_module });
    b.installArtifact(executable);

    // Reading the emitted image back is what turns "the link exited zero" into
    // evidence about the Mach-O the #899 rule is actually about.
    b.step(
        "verify-host-artifact",
        "Check the emitted binary's architecture, deployment target and libSystem linkage (#899)",
    ).dependOn(ra8_build.addVerifyHostArtifactStep(b, executable));

    const run_app = b.addRunArtifact(executable);
    if (b.args) |args| run_app.addArgs(args);
    b.step("run", "Create deliberately degraded JPEG levels").dependOn(&run_app.step);

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("image_pyramid", app_module);
    const tests = b.addTest(.{ .root_module = test_module });

    const test_step = b.step("test", "Run image pyramid tests");
    const run_tests = b.addRunArtifact(tests);
    test_step.dependOn(&run_tests.step);
    ra8_build.allowForeignHostTests(test_step, tests, run_tests);
}
