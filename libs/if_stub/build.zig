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
const ra8_build = @import("ra8_zig_build");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{
        .default_target = ra8_build.hostDefaultTargetQuery(b),
    });
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

    // Slice-5 proof image (RA8FW-976): the complete set bound through the
    // real `libs/if` facade in one freestanding image. It builds only for
    // freestanding targets, where the portability gate's --image-builds
    // phase picks it up on the default step; the host build is unchanged.
    // The image carries its own compiler_rt: it is a zig-native image, not
    // a CMake-linked firmware link.
    if (target.result.os.tag == .freestanding) {
        const proof_module = b.createModule(.{
            .root_source_file = b.path("src/proof_image.zig"),
            .target = target,
            .optimize = optimize,
        });
        proof_module.addImport("clock_stub_abi", stub_abi);
        proof_module.addImport("clock_stub_ops_abi", ops_abi);
        proof_module.addImport("rng_stub_abi", rng_abi);
        proof_module.addImport("gpio_stub_abi", gpio_abi);
        proof_module.addImport("uart_stub_abi", uart_abi);

        // The facade as one module composes the peripheral facades so the image
        // carries one copy of the shared core and every facade export resolves.
        const if_dep_proof = b.dependency("if", .{
            .target = target,
            .optimize = optimize,
        });
        proof_module.addImport("fw_if", if_dep_proof.module("fw_if"));

        const proof = b.addExecutable(.{
            .name = "if_stub_proof",
            .root_module = proof_module,
        });
        const install_proof = b.addInstallArtifact(proof, .{});
        b.default_step.dependOn(&install_proof.step);
    }

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
    const if_dep = b.dependency("if", .{
        .target = target,
        .optimize = optimize,
    });
    const if_lib = if_dep.artifact("if");
    const core_lib = b.dependency("ra8_core", .{
        .target = target,
        .optimize = optimize,
    }).artifact("ra8_core");
    const if_include_dir = if_lib.getEmittedIncludeTree();
    const core_include_dir = core_lib.getEmittedIncludeTree();

    const smoke_specs = [_]struct { name: []const u8, file: []const u8 }{
        .{ .name = "if_stub_clock_smoke", .file = "tests/clock_stub_smoke.c" },
        .{ .name = "if_stub_rng_smoke", .file = "tests/rng_stub_smoke.c" },
        .{ .name = "if_stub_gpio_smoke", .file = "tests/gpio_stub_smoke.c" },
        .{ .name = "if_stub_uart_smoke", .file = "tests/uart_stub_smoke.c" },
    };
    for (smoke_specs) |spec| {
        const obj = b.addObject(.{
            .name = spec.name,
            .root_module = b.createModule(.{
                .target = target,
                .optimize = optimize,
            }),
        });
        obj.root_module.addCSourceFile(.{
            .file = b.path(spec.file),
            .flags = &.{"-std=c11"},
        });
        obj.root_module.addIncludePath(b.path("inc"));
        obj.root_module.addIncludePath(if_include_dir);
        obj.root_module.addIncludePath(core_include_dir);
        b.default_step.dependOn(&obj.step);
    }

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
    library.installHeadersDirectory(b.path("inc"), "", .{});
    b.installArtifact(library);
}
