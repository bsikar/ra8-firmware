//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_dfu`.
//!
//! Every seam of this library is Zig: the polled USB-DFU host driver,
//! the pure boot logic the bootloader runs at reset, and
//! the MRAM slot programmer. The USBX device class glue is its own archive,
//! `libs/ra8_dfu_device`, so linking it stays the opt-in to USBX.
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
const ra8_build = @import("ra8_zig_build");

/// Every unit under `src/internal/`, in dependency order, with the units it
/// imports. Adding a unit means adding one row here.
const units = [_]struct { name: []const u8, imports: []const []const u8 }{
    .{ .name = "crc32", .imports = &.{} },
    .{ .name = "err", .imports = &.{} },
    .{ .name = "image", .imports = &.{} },
    .{ .name = "launch", .imports = &.{"image"} },
    .{ .name = "slot", .imports = &.{} },
    .{ .name = "program", .imports = &.{ "image", "slot" } },
    .{ .name = "boot_state", .imports = &.{ "crc32", "image", "slot" } },
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
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    var modules = std.StringHashMap(*std.Build.Module).init(b.allocator);

    const update_dep = b.dependency("ra8_update", .{
        .target = target,
        .optimize = optimize,
    });
    const upd_err = update_dep.module("err");
    const upd_pipeline = update_dep.module("pipeline");
    const upd_session = update_dep.module("session");
    const upd_bundle = update_dep.module("bundle");

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

    const dfu_pipeline_module = b.createModule(.{
        .root_source_file = b.path("src/internal/dfu_pipeline.zig"),
        .target = target,
        .optimize = optimize,
    });
    dfu_pipeline_module.addImport("err", modules.get("err").?);
    dfu_pipeline_module.addImport("image", modules.get("image").?);
    dfu_pipeline_module.addImport("program", modules.get("program").?);
    dfu_pipeline_module.addImport("slot", modules.get("slot").?);
    dfu_pipeline_module.addImport("upd_err", upd_err);
    dfu_pipeline_module.addImport("pipeline", upd_pipeline);
    dfu_pipeline_module.addImport("upd_session", upd_session);
    dfu_pipeline_module.addImport("bundle", upd_bundle);

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

    const mram_port_module = b.createModule(.{
        .root_source_file = b.path("src/mram_port.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "err", "program" }) |name| {
        mram_port_module.addImport(name, modules.get(name).?);
    }

    const program_abi_module = b.createModule(.{
        .root_source_file = b.path("src/program_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "err", "image", "program", "slot" }) |name| {
        program_abi_module.addImport(name, modules.get(name).?);
    }
    program_abi_module.addImport("mram_port", mram_port_module);
    program_abi_module.addImport("dfu_pipeline", dfu_pipeline_module);
    program_abi_module.addImport("upd_session", upd_session);

    const boot_state_abi_module = b.createModule(.{
        .root_source_file = b.path("src/boot_state_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "boot_state", "err", "slot" }) |name| {
        boot_state_abi_module.addImport(name, modules.get(name).?);
    }
    boot_state_abi_module.addImport("mram_port", mram_port_module);
    boot_state_abi_module.addImport("dfu_pipeline", dfu_pipeline_module);

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("dfu_host_abi", abi_module);
    root_module.addImport("dfu_boot_abi", boot_abi_module);
    root_module.addImport("launch_abi", launch_abi_module);
    root_module.addImport("program_abi", program_abi_module);
    root_module.addImport("boot_state_abi", boot_state_abi_module);

    const library = b.addLibrary(.{
        .name = "ra8_dfu",
        .linkage = .static,
        .root_module = root_module,
    });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
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
    boot_root_module.addImport("program_abi", program_abi_module);
    boot_root_module.addImport("boot_state_abi", boot_state_abi_module);
    const boot_library = b.addLibrary(.{
        .name = "ra8_dfu_boot",
        .linkage = .static,
        .root_module = boot_root_module,
    });
    boot_library.bundle_compiler_rt = boot_library.root_module.resolved_target.?.result.os.tag != .freestanding;
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
    program_test_module.addImport("dfu_pipeline", dfu_pipeline_module);
    program_test_module.addImport("upd_err", upd_err);
    program_test_module.addImport("pipeline", upd_pipeline);
    program_test_module.addImport("upd_session", upd_session);
    program_test_module.addImport("bundle", upd_bundle);

    const boot_state_test_module = b.createModule(.{
        .root_source_file = b.path("tests/boot_state_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    inline for (.{ "boot_state", "image", "slot" }) |name| {
        boot_state_test_module.addImport(name, modules.get(name).?);
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
    const boot_state_tests = b.addTest(.{ .root_module = boot_state_test_module });
    test_step.dependOn(&b.addRunArtifact(boot_state_tests).step);

    // Host C ABI acceptance suite for ra8_dfu_host_run / ra8_dfu_host_program.
    // Built only for hosted targets where libc and test mocks exist.
    if (target.result.os.tag != .freestanding) {
        const c_host_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
            .link_libc = true,
        });
        c_host_module.addIncludePath(b.path("inc"));
        c_host_module.addIncludePath(b.path("../ra8_core/inc"));
        c_host_module.addIncludePath(b.path("../ra8_hal/inc"));
        c_host_module.addCSourceFile(.{
            .file = b.path("tests/src/test_dfu_host.c"),
            .flags = &.{ "-std=c23", "-Wall", "-Wextra", "-Werror" },
        });
        const c_host_test = b.addExecutable(.{
            .name = "test_dfu_host",
            .root_module = c_host_module,
        });
        c_host_test.root_module.linkLibrary(library);
        const run_c_host = b.addRunArtifact(c_host_test);
        run_c_host.expectExitCode(0);
        test_step.dependOn(&run_c_host.step);
    }
}
