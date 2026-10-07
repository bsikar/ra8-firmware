//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const ra8_build = @import("ra8_zig_build");
const Translator = @import("translate_c").Translator;

/// Both halves of the codec are Zig, and so is ra8_core's error handler,
/// so nothing here is compiled from C: the codec arrives as the `ra8_jpeg`
/// and `ra8_core` archives linked in build(). Only the public
/// ra8_jpeg_sw.h is translated, for src/codec.zig.
fn addCodec(module: *std.Build.Module, b: *std.Build) void {
    const header = b.addWriteFiles().add("ra8_jpeg_sw_c.h", "#include <stdbool.h>\n#include \"ra8_jpeg_sw.h\"\n");
    const translator: Translator = .init(b.dependency("translate_c", .{}), .{
        .c_source_file = header,
        .target = module.resolved_target.?,
        .optimize = module.optimize.?,
        .link_libc = true,
    });
    translator.addIncludePath(b.path("../../../libs/ra8_jpeg/inc"));
    translator.addIncludePath(b.path("../../../libs/ra8_core/inc"));
    module.addImport("ra8_jpeg_sw_h", translator.mod);
    module.link_libc = true;
}

pub fn build(b: *std.Build) void {
    // Default target comes from the shared host probe so a native arm64 macOS
    // build links Zig's bundled libSystem stub instead of the SDK's (RA8FW-330).
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

    const core = b.dependency("ra8_core", .{ .target = target, .optimize = optimize });
    app_module.linkLibrary(core.artifact("ra8_core_zig"));

    const executable = b.addExecutable(.{ .name = "image_pyramid", .root_module = app_module });
    b.installArtifact(executable);

    // Reading the emitted image back is what turns "the link exited zero" into
    // evidence about the Mach-O the RA8FW-330 rule is actually about.
    b.step(
        "verify-host-artifact",
        "Check the emitted binary's architecture, deployment target and libSystem linkage (RA8FW-330)",
    ).dependOn(ra8_build.addVerifyHostArtifactStep(b, executable));

    const run_app = b.addRunArtifact(executable);
    run_app.addPassthruArgs();
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
