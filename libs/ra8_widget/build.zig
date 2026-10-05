//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_widget`, which is now Zig end to end. The library's
//! public C ABI (`inc/ra8_widget.h`) is unchanged; this archive carries the
//! module-private paint helpers that `src/ra8_widget_internal.h` declares,
//! nine leaf widgets, the container panel that nests them into a tree, and
//! the flat container ops every one of them dispatches through. No C
//! translation unit is left in `libs/ra8_widget/src`.
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

    const host_paint_module = b.createModule(.{
        .root_source_file = b.path("src/host_paint.zig"),
        .target = target,
        .optimize = optimize,
    });
    const host_render_test_module = b.createModule(.{
        .root_source_file = b.path("tests/host_render_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    host_render_test_module.addImport("host", host_paint_module);
    host_render_test_module.addImport("abi", b.createModule(.{
        .root_source_file = b.path("src/ra8_widget_abi.zig"),
        .target = target,
        .optimize = optimize,
    }));
    const host_render_tests = b.addTest(.{ .root_module = host_render_test_module });

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

    const toolbar_module = b.createModule(.{
        .root_source_file = b.path("src/widget_toolbar_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const toolbar_test_module = b.createModule(.{
        .root_source_file = b.path("tests/toolbar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    toolbar_test_module.addImport("abi", toolbar_module);
    const toolbar_tests = b.addTest(.{ .root_module = toolbar_test_module });

    const keyboard_module = b.createModule(.{
        .root_source_file = b.path("src/widget_keyboard_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const keyboard_test_module = b.createModule(.{
        .root_source_file = b.path("tests/keyboard_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    keyboard_test_module.addImport("abi", keyboard_module);
    const keyboard_tests = b.addTest(.{ .root_module = keyboard_test_module });

    const nav_bar_module = b.createModule(.{
        .root_source_file = b.path("src/widget_nav_bar_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const nav_bar_test_module = b.createModule(.{
        .root_source_file = b.path("tests/nav_bar_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    nav_bar_test_module.addImport("abi", nav_bar_module);
    const nav_bar_tests = b.addTest(.{ .root_module = nav_bar_test_module });

    const panel_module = b.createModule(.{
        .root_source_file = b.path("src/widget_panel_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const panel_test_module = b.createModule(.{
        .root_source_file = b.path("tests/panel_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    panel_test_module.addImport("abi", panel_module);
    const panel_tests = b.addTest(.{ .root_module = panel_test_module });

    const reflow_view_module = b.createModule(.{
        .root_source_file = b.path("src/widget_reflow_view_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const reflow_view_test_module = b.createModule(.{
        .root_source_file = b.path("tests/reflow_view_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    reflow_view_test_module.addImport("abi", reflow_view_module);
    const reflow_view_tests = b.addTest(.{ .root_module = reflow_view_test_module });

    const core_module = b.createModule(.{
        .root_source_file = b.path("src/widget_core_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const core_test_module = b.createModule(.{
        .root_source_file = b.path("tests/core_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    core_test_module.addImport("abi", core_module);
    const core_tests = b.addTest(.{ .root_module = core_test_module });

    const book_module = b.createModule(.{
        .root_source_file = b.path("src/widget_book_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const book_test_module = b.createModule(.{
        .root_source_file = b.path("tests/book_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    book_test_module.addImport("abi", book_module);
    const book_tests = b.addTest(.{ .root_module = book_test_module });

    const run_host_render_tests = b.addRunArtifact(host_render_tests);
    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_label_tests = b.addRunArtifact(label_tests);
    const run_button_tests = b.addRunArtifact(button_tests);
    const run_progress_bar_tests = b.addRunArtifact(progress_bar_tests);
    const run_status_bar_tests = b.addRunArtifact(status_bar_tests);
    const run_toolbar_tests = b.addRunArtifact(toolbar_tests);
    const run_keyboard_tests = b.addRunArtifact(keyboard_tests);
    const run_nav_bar_tests = b.addRunArtifact(nav_bar_tests);
    const run_panel_tests = b.addRunArtifact(panel_tests);
    const run_reflow_view_tests = b.addRunArtifact(reflow_view_tests);
    const run_book_tests = b.addRunArtifact(book_tests);
    const run_core_tests = b.addRunArtifact(core_tests);
    const test_step = b.step("test", "Run Zig ra8_widget tests");
    test_step.dependOn(&run_host_render_tests.step);
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_label_tests.step);
    test_step.dependOn(&run_button_tests.step);
    test_step.dependOn(&run_progress_bar_tests.step);
    test_step.dependOn(&run_status_bar_tests.step);
    test_step.dependOn(&run_toolbar_tests.step);
    test_step.dependOn(&run_keyboard_tests.step);
    test_step.dependOn(&run_nav_bar_tests.step);
    test_step.dependOn(&run_panel_tests.step);
    test_step.dependOn(&run_reflow_view_tests.step);
    test_step.dependOn(&run_book_tests.step);
    test_step.dependOn(&run_core_tests.step);
}
