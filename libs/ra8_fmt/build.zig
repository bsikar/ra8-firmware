//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `ra8_fmt`: portable callback-driven streaming JOF and RABOOK
//! engines plus the byte-stream contract headers (`inc/ra8_fmt_io.h` and
//! `inc/ra8_fmt_stream.h`).

const std = @import("std");
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
    const optimize = b.standardOptimizeOption(.{});

    const c_abi_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/c_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const fmt_report_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/fmt_report.zig"),
        .target = target,
        .optimize = optimize,
    });
    fmt_report_mod.addImport("c_abi", c_abi_mod);

    const source_io_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/source_io.zig"),
        .target = target,
        .optimize = optimize,
    });
    source_io_mod.addImport("c_abi", c_abi_mod);

    const inspect_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/inspect.zig"),
        .target = target,
        .optimize = optimize,
    });
    inspect_mod.addImport("c_abi", c_abi_mod);
    inspect_mod.addImport("fmt_report", fmt_report_mod);
    inspect_mod.addImport("source_io", source_io_mod);

    const rabook_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/rabook.zig"),
        .target = target,
        .optimize = optimize,
    });
    rabook_mod.addImport("c_abi", c_abi_mod);
    rabook_mod.addImport("fmt_report", fmt_report_mod);
    rabook_mod.addImport("source_io", source_io_mod);

    const convert_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/convert.zig"),
        .target = target,
        .optimize = optimize,
    });
    convert_mod.addImport("c_abi", c_abi_mod);
    convert_mod.addImport("fmt_report", fmt_report_mod);
    convert_mod.addImport("source_io", source_io_mod);

    const verify_plan_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/verify_plan.zig"),
        .target = target,
        .optimize = optimize,
    });
    verify_plan_mod.addImport("c_abi", c_abi_mod);
    verify_plan_mod.addImport("convert", convert_mod);
    verify_plan_mod.addImport("source_io", source_io_mod);

    const verify_mod = b.createModule(.{
        .root_source_file = b.path("src/internal/verify.zig"),
        .target = target,
        .optimize = optimize,
    });
    verify_mod.addImport("c_abi", c_abi_mod);
    verify_mod.addImport("fmt_report", fmt_report_mod);
    verify_mod.addImport("source_io", source_io_mod);

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/ra8_fmt_abi.zig"),
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    library_module.addImport("c_abi", c_abi_mod);
    library_module.addImport("inspect", inspect_mod);
    library_module.addImport("rabook", rabook_mod);
    library_module.addImport("convert", convert_mod);
    library_module.addImport("verify_plan", verify_plan_mod);
    library_module.addImport("verify", verify_mod);

    const library = b.addLibrary(.{
        .name = "ra8_fmt",
        .linkage = .static,
        .root_module = library_module,
    });
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    library.link_function_sections = true;
    library.link_data_sections = true;
    library.root_module.pic = true;
    b.installArtifact(library);

    // Keep the cross-target C contract header smoke compile on default_step.
    const smoke_module = b.createModule(.{
        .target = target,
        .optimize = optimize,
    });
    smoke_module.addIncludePath(b.path("inc"));
    smoke_module.addIncludePath(b.path("../ra8_core/inc"));
    smoke_module.addCSourceFile(.{
        .file = b.path("tests/ra8_fmt_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    const smoke = b.addObject(.{
        .name = "ra8_fmt_smoke",
        .root_module = smoke_module,
    });
    b.default_step.dependOn(&smoke.step);

    b.default_step.dependOn(&b.addInstallDirectory(.{
        .source_dir = b.path("inc"),
        .install_dir = .header,
        .install_subdir = "",
    }).step);

    const mocks_mod = b.createModule(.{
        .root_source_file = b.path("tests/test_mocks.zig"),
        .target = target,
        .optimize = optimize,
    });
    mocks_mod.addImport("c_abi", c_abi_mod);

    const units = [_]struct { root: []const u8, mod_name: ?[]const u8, mod: ?*std.Build.Module }{
        .{ .root = "tests/inspect_test.zig", .mod_name = "inspect", .mod = inspect_mod },
        .{ .root = "tests/rabook_test.zig", .mod_name = "rabook", .mod = rabook_mod },
        .{ .root = "tests/convert_test.zig", .mod_name = "convert", .mod = convert_mod },
        .{ .root = "tests/verify_plan_test.zig", .mod_name = "verify_plan", .mod = verify_plan_mod },
        .{ .root = "tests/verify_test.zig", .mod_name = "verify", .mod = verify_mod },
        .{ .root = "tests/abi_test.zig", .mod_name = "ra8_fmt_abi", .mod = library_module },
    };

    const test_step = b.step("test", "Run Zig ra8_fmt unit and ABI tests");
    for (units) |unit| {
        const tmod = b.createModule(.{
            .root_source_file = b.path(unit.root),
            .target = target,
            .optimize = optimize,
        });
        tmod.addImport("c_abi", c_abi_mod);
        if (unit.mod_name) |mname| {
            tmod.addImport(mname, unit.mod.?);
        }
        if (!std.mem.eql(u8, unit.root, "tests/abi_test.zig")) {
            tmod.addImport("mocks", mocks_mod);
        }
        const tests = b.addTest(.{ .root_module = tmod });
        const run_tests = b.addRunArtifact(tests);
        ra8_build.configureHostTestRun(b, test_step, tests, run_tests);
        test_step.dependOn(&run_tests.step);
    }
}
