//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

/// The decoder half of the codec is still C and is compiled straight in; the
/// encoder is Zig as of #2795 and arrives as the `ra8_jpeg` archive, which
/// also carries the `ra8_imgdec` backend. The C this app compiles supplies the
/// decode symbols that backend externs, so the two halves resolve each other.
fn addCodec(module: *std.Build.Module, b: *std.Build) void {
    module.addIncludePath(b.path("../../../libs/ra8_jpeg/inc"));
    module.addIncludePath(b.path("../../../libs/ra8_jpeg/src"));
    module.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    module.addCSourceFiles(.{
        .files = &.{
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_decode.c",
            "../../../libs/ra8_jpeg/src/ra8_jpeg_sw_stream.c",
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

    const jpeg = b.dependency("ra8_jpeg", .{ .target = target, .optimize = optimize });
    app_module.linkLibrary(jpeg.artifact("ra8_jpeg"));

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
    _ = ra8_build.addHostTestRun(b, test_step, tests);
}
