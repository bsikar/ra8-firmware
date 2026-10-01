//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_dfu`.
//!
//! Three seams of this library are Zig: the polled USB-DFU host driver,
//! the pure boot logic the bootloader runs at reset, and
//! the MRAM slot programmer. Only the USBX device class is still C,
//! which `.github/zig-parallel-tree-allowlist.tsv` records per file.
//!
//! The slot programmer is the one unit with a placement requirement: its
//! exports are in `.sram_text` so the program loop does not execute from the
//! array it is writing. See src/program_abi.zig.
//!
//! The root of trust moved out to `libs/ra8_rot`: it is its own
//! archive, and an archive is only linkable where cmake can find a
//! `build.zig` under `libs/<name>`.
//!
//! Each unit is its own module so the tests can import the same module
//! objects the archive does. That is what lets the tests drive the DFU
//! sequence against a scripted device without linking a USB controller:
//! the step modules are generic over their HAL, and only
//! `dfu_host_abi` binds the real one.

const std = @import("std");

/// Every unit under `src/internal/`, in dependency order, with the units it
/// imports. Adding a unit means adding one row here.
const units = [_]struct { name: []const u8, imports: []const []const u8 }{
    .{ .name = "crc32", .imports = &.{} },
    .{ .name = "err", .imports = &.{} },
    .{ .name = "image", .imports = &.{} },
    .{ .name = "launch", .imports = &.{"image"} },
    .{ .name = "slot", .imports = &.{} },
    .{ .name = "program", .imports = &.{ "image", "slot" } },
    .{ .name = "proto", .imports = &.{} },
    .{ .name = "tune", .imports = &.{} },
    .{ .name = "hal", .imports = &.{"err"} },
    .{ .name = "control", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "attach", .imports = &.{ "control", "err", "hal", "proto", "tune" } },
    .{ .name = "status", .imports = &.{ "err", "hal", "proto", "tune" } },
    .{ .name = "download", .imports = &.{ "err", "hal", "proto", "status" } },
    .{ .name = "verify", .imports = &.{ "download", "err", "hal", "proto" } },
    .{ .name = "session", .imports = &.{ "attach", "control", "download", "err", "hal", "proto", "verify" } },
};

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    var modules = std.StringHashMap(*std.Build.Module).init(b.allocator);

    inline for (units) |unit| {
        const module = b.createModule(.{
            .root_source_file = b.path(b.fmt("src/internal/{s}.zig", .{unit.name})),
            .target = target,
            .optimize = optimize,
        });
        for (unit.imports) |dependency| {
            module.addImport(dependency, modules.get(dependency).?);
        }
        modules.put(unit.name, module) catch @panic("OOM");
    }

    const abi_module = b.createModule(.{
        .root_source_file = b.path("src/dfu_host_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "err", "hal", "proto", "session" }) |name| {
        abi_module.addImport(name, modules.get(name).?);
    }

    const boot_abi_module = b.createModule(.{
        .root_source_file = b.path("src/dfu_boot_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "crc32", "image", "slot" }) |name| {
        boot_abi_module.addImport(name, modules.get(name).?);
    }

    const launch_abi_module = b.createModule(.{
        .root_source_file = b.path("src/launch_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "image", "launch" }) |name| {
        launch_abi_module.addImport(name, modules.get(name).?);
    }

    const program_abi_module = b.createModule(.{
        .root_source_file = b.path("src/program_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "image", "program", "slot" }) |name| {
        program_abi_module.addImport(name, modules.get(name).?);
    }

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("dfu_host_abi", abi_module);
    root_module.addImport("dfu_boot_abi", boot_abi_module);
    root_module.addImport("launch_abi", launch_abi_module);
    root_module.addImport("program_abi", program_abi_module);

    const library = b.addLibrary(.{
        .name = "ra8_dfu",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    library.bundle_compiler_rt = true;
    b.installArtifact(library);

    // The boot logic on its own, for links that cannot resolve the host
    // driver's `ra8_usb_host_*` seam. See src/boot_root.zig.
    const boot_root_module = b.createModule(.{
        .root_source_file = b.path("src/boot_root.zig"),
        .target = target,
        .optimize = optimize,
    });
    boot_root_module.addImport("dfu_boot_abi", boot_abi_module);
    boot_root_module.addImport("launch_abi", launch_abi_module);
    const boot_library = b.addLibrary(.{
        .name = "ra8_dfu_boot",
        .linkage = .static,
        .root_module = boot_root_module,
    });
    boot_library.bundle_compiler_rt = true;
    b.installArtifact(boot_library);

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/dfu_host_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (units) |unit| {
        test_module.addImport(unit.name, modules.get(unit.name).?);
    }

    const boot_test_module = b.createModule(.{
        .root_source_file = b.path("tests/boot_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "crc32", "image", "slot" }) |name| {
        boot_test_module.addImport(name, modules.get(name).?);
    }

    const launch_test_module = b.createModule(.{
        .root_source_file = b.path("tests/launch_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "image", "launch" }) |name| {
        launch_test_module.addImport(name, modules.get(name).?);
    }

    const program_test_module = b.createModule(.{
        .root_source_file = b.path("tests/program_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "image", "program", "slot" }) |name| {
        program_test_module.addImport(name, modules.get(name).?);
    }

    const test_step = b.step("test", "Run Zig ra8_dfu tests");
    const tests = b.addTest(.{ .root_module = test_module });
    test_step.dependOn(&b.addRunArtifact(tests).step);
    const boot_tests = b.addTest(.{ .root_module = boot_test_module });
    test_step.dependOn(&b.addRunArtifact(boot_tests).step);
    const launch_tests = b.addTest(.{ .root_module = launch_test_module });
    test_step.dependOn(&b.addRunArtifact(launch_tests).step);
    const program_tests = b.addTest(.{ .root_module = program_test_module });
    test_step.dependOn(&b.addRunArtifact(program_tests).step);
}
