//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig half of `ra8_widget`. The library's public C ABI
//! (`inc/ra8_widget.h`) is unchanged; this archive carries the module-private
//! paint helpers that `src/ra8_widget_internal.h` declares plus the text-label,
//! push-button, progress-bar and status-bar leaf widgets, so the sibling widget
//! translation units link them instead of compiling `ra8_widget_paint.c` /
//! `_label.c` / `_button.c` / `_progress_bar.c` / `_status_bar.c`.
//! The `test` step verifies the pure geometry and each membrane.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library = b.addLibrary(.{
        .name = "ra8_widget",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_widget_abi.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    // The host C test executables are linked by the system toolchain, not by
    // `zig cc`, so nothing else on that link line provides Zig's runtime
    // helpers. Without this the archive leaves `__zig_probe_stack` undefined.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/paint.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/widget_paint_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    const internal_test_module = b.createModule(.{
        .root_source_file = b.path("tests/internal_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    internal_test_module.addImport("implementation", implementation_module);
    const internal_tests = b.addTest(.{ .root_module = internal_test_module });

    const abi_test_module = b.createModule(.{
        .root_source_file = b.path("tests/abi_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_test_module.addImport("abi", abi_module);
    const abi_tests = b.addTest(.{ .root_module = abi_test_module });

    const label_module = b.createModule(.{
        .root_source_file = b.path("src/widget_label_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const label_test_module = b.createModule(.{
        .root_source_file = b.path("tests/label_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    label_test_module.addImport("abi", label_module);
    const label_tests = b.addTest(.{ .root_module = label_test_module });

    const button_module = b.createModule(.{
        .root_source_file = b.path("src/widget_button_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const button_test_module = b.createModule(.{
        .root_source_file = b.path("tests/button_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    button_test_module.addImport("abi", button_module);
    const button_tests = b.addTest(.{ .root_module = button_test_module });

    const progress_bar_module = b.createModule(.{
        .root_source_file = b.path("src/widget_progress_bar_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const progress_bar_test_module = b.createModule(.{
        .root_source_file = b.path("tests/progress_bar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    progress_bar_test_module.addImport("abi", progress_bar_module);
    const progress_bar_tests = b.addTest(.{ .root_module = progress_bar_test_module });

    const status_bar_module = b.createModule(.{
        .root_source_file = b.path("src/widget_status_bar_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const status_bar_test_module = b.createModule(.{
        .root_source_file = b.path("tests/status_bar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    status_bar_test_module.addImport("abi", status_bar_module);
    const status_bar_tests = b.addTest(.{ .root_module = status_bar_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_label_tests = b.addRunArtifact(label_tests);
    const run_button_tests = b.addRunArtifact(button_tests);
    const run_progress_bar_tests = b.addRunArtifact(progress_bar_tests);
    const run_status_bar_tests = b.addRunArtifact(status_bar_tests);
    const test_step = b.step("test", "Run Zig ra8_widget tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_label_tests.step);
    test_step.dependOn(&run_button_tests.step);
    test_step.dependOn(&run_progress_bar_tests.step);
    test_step.dependOn(&run_status_bar_tests.step);
}
