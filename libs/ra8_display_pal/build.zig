//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of `ra8_display_pal`: the dispatcher
//! in `inc/ra8_display_pal.h`, the page-turn refresh policy in
//! `inc/ra8_display_pal_policy.h`, and the GLCDC/LCD backend in
//! `inc/ra8_display_pal_lcd.h`. CMake consumes the installed static library
//! through those unchanged C headers; the `test` step covers the pure decision
//! logic and the ABI membrane.
//!
//! Two options, both for the LCD backend, because one archive per CPU is
//! linked into every app and the C carried these as per-app preprocessor
//! flags: `off-target` drops the ARM barrier for the host, and
//! `boot-enable-cache-mpu` keeps the D-cache clean a cacheable framebuffer
//! needs before the GLCDC scans it.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const off_target = b.option(
        bool,
        "off-target",
        "Skip the ARM DSB the GLCDC's AXI master needs, for the host build",
    ) orelse (target.result.os.tag != .freestanding);

    // Safe direction by default: cleaning the D-cache when no cache is enabled
    // costs cycles, while skipping it when one is shows stale pixels.
    const boot_enable_cache_mpu = b.option(
        bool,
        "boot-enable-cache-mpu",
        "Clean painted framebuffer rows out of the write-back L1 D-cache on flush and clear",
    ) orelse (target.result.os.tag == .freestanding);

    const build_options = b.addOptions();
    build_options.addOption(bool, "off_target", off_target);
    build_options.addOption(bool, "boot_enable_cache_mpu", boot_enable_cache_mpu);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_display_pal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_display_pal",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const implementation_module = b.createModule(.{
        .root_source_file = b.path("src/internal/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_display_pal_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    abi_module.addOptions("build_config", build_options);
    const lcd_module = b.createModule(.{
        .root_source_file = b.path("src/internal/lcd.zig"),
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

    const lcd_test_module = b.createModule(.{
        .root_source_file = b.path("tests/lcd_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    lcd_test_module.addImport("implementation", lcd_module);
    const lcd_tests = b.addTest(.{ .root_module = lcd_test_module });

    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_lcd_tests = b.addRunArtifact(lcd_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const test_step = b.step("test", "Run Zig ra8_display_pal tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_lcd_tests.step);
    test_step.dependOn(&run_abi_tests.step);
}
