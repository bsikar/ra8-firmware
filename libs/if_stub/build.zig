//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `if_stub`, the proof binding behind the neutral `fw_if_*`
//! ports (epic RA8FW-618 slice 5).
//!
//! Two membranes over one pure profile, in the shape `if_ra8_cgc` set for a
//! chip adapter: `clock_stub_abi` is the profile's own export, and
//! `clock_stub_ops_abi` is the three ops, which reach `fw_clock_bind` as an
//! extern resolved at the final link. The whole profile runs on the host
//! without a peripheral block, so both layers have Zig tests here; a C suite
//! over the facade comes with the first C consumer of the set.
//!
//! The smoke translation unit is also part of the default step, so a cross
//! compile proves the header itself the way ra8_fmt's does.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const clock_stub = b.createModule(.{
        .root_source_file = b.path("src/internal/clock_stub.zig"),
        .target = target,
        .optimize = optimize,
    });

    const stub_abi = b.createModule(.{
        .root_source_file = b.path("src/clock_stub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    stub_abi.addImport("clock_stub", clock_stub);

    const rng_stub = b.createModule(.{
        .root_source_file = b.path("src/internal/rng_stub.zig"),
        .target = target,
        .optimize = optimize,
    });

    const rng_abi = b.createModule(.{
        .root_source_file = b.path("src/rng_stub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    rng_abi.addImport("rng_stub", rng_stub);

    const gpio_stub = b.createModule(.{
        .root_source_file = b.path("src/internal/gpio_stub.zig"),
        .target = target,
        .optimize = optimize,
    });

    const gpio_abi = b.createModule(.{
        .root_source_file = b.path("src/gpio_stub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    gpio_abi.addImport("gpio_stub", gpio_stub);

    const uart_stub = b.createModule(.{
        .root_source_file = b.path("src/internal/uart_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    const uart_abi = b.createModule(.{
        .root_source_file = b.path("src/uart_stub_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    uart_abi.addImport("uart_stub", uart_stub);
    const ops_abi = b.createModule(.{
        .root_source_file = b.path("src/clock_stub_ops_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    ops_abi.addImport("clock_stub_abi", stub_abi);

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    root_module.addImport("clock_stub_abi", stub_abi);
    root_module.addImport("clock_stub_ops_abi", ops_abi);
    root_module.addImport("rng_stub_abi", rng_abi);
    root_module.addImport("gpio_stub_abi", gpio_abi);
    root_module.addImport("uart_stub_abi", uart_abi);

    const test_step = b.step("test", "Run Zig if_stub tests");

    const unit = b.createModule(.{
        .root_source_file = b.path("src/internal/clock_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = unit })).step);

    const rng_unit = b.createModule(.{
        .root_source_file = b.path("src/internal/rng_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = rng_unit })).step);

    const gpio_unit = b.createModule(.{
        .root_source_file = b.path("src/internal/gpio_stub.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = gpio_unit })).step);

    const ops_test = b.createModule(.{
        .root_source_file = b.path("tests/clock_stub_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    ops_test.addImport("clock_stub", clock_stub);
    ops_test.addImport("clock_stub_abi", stub_abi);
    ops_test.addImport("clock_stub_ops_abi", ops_abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = ops_test })).step);

    const rng_test = b.createModule(.{
        .root_source_file = b.path("tests/rng_stub_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    rng_test.addImport("rng_stub", rng_stub);
    rng_test.addImport("rng_stub_abi", rng_abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = rng_test })).step);

    const gpio_test = b.createModule(.{
        .root_source_file = b.path("tests/gpio_stub_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    gpio_test.addImport("gpio_stub", gpio_stub);
    gpio_test.addImport("gpio_stub_abi", gpio_abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = gpio_test })).step);

    const uart_test = b.createModule(.{
        .root_source_file = b.path("tests/uart_stub_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    uart_test.addImport("uart_stub", uart_stub);
    uart_test.addImport("uart_stub_abi", uart_abi);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = uart_test })).step);
    const smoke = b.addObject(.{
        .name = "if_stub_clock_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/clock_stub_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    smoke.root_module.addIncludePath(b.path("inc"));
    smoke.root_module.addIncludePath(b.path("../if/inc"));
    smoke.root_module.addIncludePath(b.path("../ra8_core/inc"));
    const rng_smoke = b.addObject(.{
        .name = "if_stub_rng_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    rng_smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/rng_stub_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    rng_smoke.root_module.addIncludePath(b.path("inc"));
    rng_smoke.root_module.addIncludePath(b.path("../if/inc"));
    rng_smoke.root_module.addIncludePath(b.path("../ra8_core/inc"));
    b.default_step.dependOn(&rng_smoke.step);

    const gpio_smoke = b.addObject(.{
        .name = "if_stub_gpio_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    gpio_smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/gpio_stub_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    gpio_smoke.root_module.addIncludePath(b.path("inc"));
    gpio_smoke.root_module.addIncludePath(b.path("../if/inc"));
    gpio_smoke.root_module.addIncludePath(b.path("../ra8_core/inc"));
    b.default_step.dependOn(&gpio_smoke.step);

    const uart_smoke = b.addObject(.{
        .name = "if_stub_uart_smoke",
        .root_module = b.createModule(.{
            .target = target,
            .optimize = optimize,
        }),
    });
    uart_smoke.root_module.addCSourceFile(.{
        .file = b.path("tests/uart_stub_smoke.c"),
        .flags = &.{"-std=c11"},
    });
    uart_smoke.root_module.addIncludePath(b.path("inc"));
    uart_smoke.root_module.addIncludePath(b.path("../if/inc"));
    uart_smoke.root_module.addIncludePath(b.path("../ra8_core/inc"));
    b.default_step.dependOn(&uart_smoke.step);
    b.default_step.dependOn(&smoke.step);

    const library = b.addLibrary(.{
        .name = "if_stub",
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
