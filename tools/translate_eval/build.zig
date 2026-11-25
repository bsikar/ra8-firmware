//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `tools/translate_eval`: host evaluation harness scoring
//! detection IoU P/R, OCR CER, and Marian chrF across float reference,
//! host int8 (`libs/ra8_translate`), and optional emulator traces.

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

    const exe_mod = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });
    exe_mod.addAnonymousImport("eval_corpus", .{
        .root_source_file = b.path("corpus/eval_corpus.json"),
    });
    exe_mod.linkLibrary(translate_lib);

    const executable = b.addExecutable(.{
        .name = "translate_eval",
        .root_module = exe_mod,
    });
    b.installArtifact(executable);

    const run_tool = b.addRunArtifact(executable);
    run_tool.step.dependOn(b.getInstallStep());
    run_tool.addPassthruArgs();
    const run_step = b.step("run", "Run multilingual translation evaluation harness");
    run_step.dependOn(&run_tool.step);

    const test_options = b.addOptions();
    test_options.addOptionPath("tool_exe", executable.getEmittedBin());

    const eval_mod = b.createModule(.{
        .root_source_file = b.path("src/cli.zig"),
        .target = target,
        .optimize = optimize,
    });
    eval_mod.addAnonymousImport("eval_corpus", .{
        .root_source_file = b.path("corpus/eval_corpus.json"),
    });
    eval_mod.linkLibrary(translate_lib);

    const eval_test_mod = b.createModule(.{
        .root_source_file = b.path("tests/eval_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    eval_test_mod.addOptions("tool_options", test_options);
    eval_test_mod.addImport("eval", eval_mod);
    eval_test_mod.linkLibrary(translate_lib);

    const eval_tests = b.addTest(.{ .root_module = eval_test_mod });
    const test_step = b.step("test", "Run translate_eval unit and CLI integration tests");
    const run_eval_tests = b.addRunArtifact(eval_tests);
    ra8_build.configureHostTestRun(b, test_step, eval_tests, run_eval_tests);
    test_step.dependOn(&run_eval_tests.step);
}
