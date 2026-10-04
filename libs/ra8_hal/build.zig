//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_hal`'s Zig half (RA8FW-497).
//!
//! ra8_hal is mid-port: most of src/ is still C, globbed into the universal
//! set every app compiles. This archive carries the units that moved to Zig
//! and is linked beside those C objects as a universal archive, the same way
//! ra8_secure_app's is, so a unit joins the link by moving here and deleting
//! its .c with no other build edit. The headers in inc/ are unchanged.

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const library = b.addLibrary(.{
        .name = "ra8_hal",
        .linkage = .static,
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/ra8_hal_abi.zig"),
            .target = target,
            .optimize = optimize,
            .pic = true,
        }),
    });
    library.bundle_compiler_rt = false;
    // One archive member per ported unit (RA8FW-542): the linker pulls only
    // the members an image references. Zig merges an object's string
    // literals into one .rodata.str1.1 that --gc-sections cannot split, so a
    // single shared object would carry every unit's log strings.
    const abi_units = [_][]const u8{ "eth", "canfd", "layer3_switch", "icu", "iwdt", "npu_quant", "glcdc_gamma", "elc", "epaper_devinfo", "eth_coma" };
    for (abi_units) |unit| {
        const object = b.addObject(.{
            .name = b.fmt("ra8_hal_{s}", .{unit}),
            .root_module = b.createModule(.{
                .root_source_file = b.path(b.fmt("src/{s}_abi.zig", .{unit})),
                .target = target,
                .optimize = optimize,
                .pic = true,
            }),
        });
        object.bundle_compiler_rt = false;
        object.link_function_sections = true;
        object.link_data_sections = true;
        library.addObject(object);
    }
    library.link_function_sections = true;
    library.link_data_sections = true;
    b.installArtifact(library);

    const test_step = b.step("test", "Run Zig ra8_hal tests");
    const units = [_]struct { name: []const u8, source: []const u8, root: []const u8 }{
        .{ .name = "eth_media", .source = "src/internal/eth_media.zig", .root = "tests/eth_media_test.zig" },
        .{ .name = "canfd_tdc", .source = "src/internal/canfd_tdc.zig", .root = "tests/canfd_tdc_test.zig" },
        .{ .name = "layer3_switch", .source = "src/internal/layer3_switch.zig", .root = "tests/layer3_switch_test.zig" },
        .{ .name = "icu", .source = "src/internal/icu.zig", .root = "tests/icu_test.zig" },
        .{ .name = "iwdt", .source = "src/internal/iwdt.zig", .root = "tests/iwdt_test.zig" },
        .{ .name = "npu_quant", .source = "src/internal/npu_quant.zig", .root = "tests/npu_quant_test.zig" },
        .{ .name = "glcdc_gamma", .source = "src/internal/glcdc_gamma.zig", .root = "tests/glcdc_gamma_test.zig" },
        .{ .name = "elc", .source = "src/internal/elc.zig", .root = "tests/elc_test.zig" },
        .{ .name = "epaper_devinfo", .source = "src/internal/epaper_devinfo.zig", .root = "tests/epaper_devinfo_test.zig" },
        .{ .name = "eth_coma", .source = "src/internal/eth_coma.zig", .root = "tests/eth_coma_test.zig" },
    };
    for (units) |unit| {
        const test_module = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        test_module.addImport(unit.name, b.createModule(.{
            .root_source_file = b.path(unit.source),
            .target = target,
            .optimize = optimize,
        }));
        const tests = b.addTest(.{ .root_module = test_module });
        test_step.dependOn(&b.addRunArtifact(tests).step);
    }
}
