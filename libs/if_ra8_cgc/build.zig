//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `if_ra8_cgc`, the RA8 chip adapter behind the neutral
//! `fw_if_clock` port.
//!
//! Two membranes over one pure table. `clock_map_abi` is the table's own
//! export; `clock_ops_abi` is the three ops, which reach `ra8_cgc`,
//! `ra8_mstp` and `fw_clock_bind` as externs resolved at the final link. Only
//! the table can run on the host without a peripheral block, so only the
//! table has a Zig test here; the ops stay covered by the untouched C suite
//! in `tests/if/src/test_fw_if_clock_ra8.c`.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const clock_map = b.createModule(.{
        .root_source_file = b.path("src/internal/clock_map.zig"),
        .target = target,
        .optimize = optimize,
    });

    const map_abi = b.createModule(.{
        .root_source_file = b.path("src/clock_map_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    map_abi.addImport("clock_map", clock_map);

    const ops_abi = b.createModule(.{
        .root_source_file = b.path("src/clock_ops_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    ops_abi.addImport("clock_map_abi", map_abi);

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("clock_map_abi", map_abi);
    root_module.addImport("clock_ops_abi", ops_abi);

    const test_step = b.step("test", "Run Zig if_ra8_cgc tests");

    const map_unit = b.createModule(.{
        .root_source_file = b.path("src/internal/clock_map.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = map_unit })).step);

    const table_test = b.createModule(.{
        .root_source_file = b.path("tests/clock_map_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    table_test.addImport("clock_map", clock_map);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = table_test })).step);

    const library = b.addLibrary(.{
        .name = "if_ra8_cgc",
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
}
