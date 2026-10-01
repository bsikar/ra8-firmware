//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig half of `ra8_board_ek_ra8d2`. CMake consumes the
//! installed static archive behind the unchanged `inc/` headers, so no
//! consumer include path moves.
//!
//! This board is only partly ported: `src/*.c` still holds the pin/LED/switch
//! core, the camera, MIPI panel and audio-USB layers, and `src/boot/`.
//! Those stay C and are recorded in the parallel-tree allowlist. The archive
//! and the remaining objects link side by side, which is why
//! `cmake/ra8_app/sources.cmake` grew a partial-port case.
//!
//! The layer calls the HAL, the chip clock binding, the io-stream sink and the
//! board's own remaining C as plain externs, resolved at link time by whatever
//! the app or the host suite already links. Nothing here needs a vendored
//! header, so there is no `@cImport`.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // The boot MPU marks the CPU0 <-> CPU1 window Normal non-cacheable only
    // under RA8_BOOT_ENABLE_CACHE_MPU, and the shared-RAM descriptor reports
    // the flag the running build carries rather than a constant.
    const boot_cache_mpu = b.option(
        bool,
        "boot-cache-mpu",
        "Build with RA8_BOOT_ENABLE_CACHE_MPU, so the shared window is Normal non-cacheable",
    ) orelse false;

    const build_options = b.addOptions();
    build_options.addOption(bool, "boot_cache_mpu", boot_cache_mpu);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    library_module.addOptions("build_config", build_options);

    const library = b.addLibrary(.{
        .name = "ra8_board_ek_ra8d2",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_board_ek_ra8d2 tests");

    // One unit module per suite, rooted at the file under test, so no internal
    // file lands in two modules of the same test binary. Each suite defines
    // the extern seam its unit calls.
    const suites = [_]struct {
        name: []const u8,
        source: []const u8,
        root: []const u8,
        needs_config: bool,
    }{
        .{
            .name = "dualcore",
            .source = "src/internal/dualcore.zig",
            .root = "tests/dualcore_test.zig",
            .needs_config = true,
        },
        .{
            .name = "usb_port",
            .source = "src/internal/usb_port.zig",
            .root = "tests/usb_port_test.zig",
            .needs_config = false,
        },
        .{
            .name = "bringup",
            .source = "src/internal/bringup.zig",
            .root = "tests/bringup_test.zig",
            .needs_config = false,
        },
        .{
            .name = "clock_profile",
            .source = "src/internal/clock_profile.zig",
            .root = "tests/clock_profile_test.zig",
            .needs_config = false,
        },
        .{
            .name = "clocks",
            .source = "src/internal/clocks.zig",
            .root = "tests/clocks_test.zig",
            .needs_config = false,
        },
        .{
            .name = "uart_console",
            .source = "src/internal/uart_console.zig",
            .root = "tests/uart_console_test.zig",
            .needs_config = false,
        },
        .{
            .name = "camera_mode",
            .source = "src/internal/camera_mode.zig",
            .root = "tests/camera_mode_test.zig",
            .needs_config = false,
        },
        .{
            .name = "eth_phy",
            .source = "src/internal/eth_phy.zig",
            .root = "tests/eth_phy_test.zig",
            .needs_config = false,
        },
        .{
            .name = "ethernet",
            .source = "src/internal/ethernet.zig",
            .root = "tests/ethernet_test.zig",
            .needs_config = false,
        },
        .{
            .name = "console_stream",
            .source = "src/internal/console_stream.zig",
            .root = "tests/console_stream_test.zig",
            .needs_config = false,
        },
    };
    for (suites) |suite| {
        const under_test = b.createModule(.{
            .root_source_file = b.path(suite.source),
            .target = target,
            .optimize = optimize,
        });
        if (suite.needs_config) under_test.addOptions("build_config", build_options);
        const test_module = b.createModule(.{
            .root_source_file = b.path(suite.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(suite.name, under_test);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
