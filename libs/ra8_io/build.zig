//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_io`'s Zig half (RA8FW-654).
//!
//! ra8_io is mid-port (log sink RA8FW-654, SDRAM backend RA8FW-697): the
//! rest of src/ is still C, globbed into every
//! app that names `ra8_io` in LIBS. sources.cmake links this archive beside
//! that C because build.zig exists, so a unit joins the link by moving here
//! and deleting its .c. The headers in inc/ are unchanged.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});
    // No unwind tables in a freestanding archive, as in ra8_hal's (RA8FW-571).
    const unwind: ?std.builtin.UnwindTables = if (target.result.os.tag == .freestanding) .none else null;

    const root = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
        .unwind_tables = unwind,
    });
    const library = b.addLibrary(.{ .name = "ra8_io", .linkage = .static, .root_module = root });
    library.bundle_compiler_rt = false;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_io tests");
    const roots = [_][]const u8{
        "tests/log_abi_test.zig",
        "tests/stream_ram_abi_test.zig",
        "tests/stream_uart_abi_test.zig",
        "tests/stream_usbcdc_abi_test.zig",
        "tests/spi_bus_spi_b_abi_test.zig",
        "tests/spi_bus_sci_spi_abi_test.zig",
        "tests/blockdev_sdram_abi_test.zig",
    };
    for (roots) |path| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(path),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport("ra8_io", root);
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
