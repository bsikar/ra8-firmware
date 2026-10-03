//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for the Zig implementation of the portable interfaces in
//! `libs/if`: the filesystem facade (`fw_if_fs`), the untrusted-name policy
//! (`ra8_path`) and the clock-intent facade (`fw_clock`). CMake consumes the installed static library
//! through the unchanged `inc/fw_if_fs.h`, `inc/fw_if_fs_types.h` and
//! `inc/fw_if_fs_backend.h` C ABI; the `test` step covers the pure guard,
//! path and coherence core plus the ABI membrane over fake backends, and
//! the untrusted-name policy behind `inc/ra8_path.h`.
//!
//! No build options: every backend below this interface is a caller-supplied
//! vtable, so nothing here is configured at compile time.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const library_module = b.createModule(.{
        .root_source_file = b.path("src/fw_if_fs_abi.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Named for the CMake library (`libs/if`), not for the C API it exposes
    // (`fw_fs_*` through `fw_if_fs.h`). The app build composes the archive it
    // links as `lib<cmake name>.a`, so an artifact called anything else is
    // simply never found. Every other migrated library already agrees with
    // its directory; this one did not, and `vfs_port_demo` could not link.
    const library = b.addLibrary(.{
        .name = "if",
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
        .root_source_file = b.path("src/fw_if_fs_abi.zig"),
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

    const policy_module = b.createModule(.{
        .root_source_file = b.path("src/internal/path.zig"),
        .target = target,
        .optimize = optimize,
    });
    const path_test_module = b.createModule(.{
        .root_source_file = b.path("tests/path_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    path_test_module.addImport("policy", policy_module);
    const path_tests = b.addTest(.{ .root_module = path_test_module });

    const clock_abi_module = b.createModule(.{
        .root_source_file = b.path("src/fw_if_clock_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const clock_test_module = b.createModule(.{
        .root_source_file = b.path("tests/clock_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    clock_test_module.addImport("abi", clock_abi_module);
    const clock_tests = b.addTest(.{ .root_module = clock_test_module });

    const timer_abi_module = b.createModule(.{
        .root_source_file = b.path("src/fw_if_timer_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const timer_test_module = b.createModule(.{
        .root_source_file = b.path("tests/timer_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    timer_test_module.addImport("abi", timer_abi_module);
    const timer_tests = b.addTest(.{ .root_module = timer_test_module });

    const pwm_abi_module = b.createModule(.{
        .root_source_file = b.path("src/fw_if_pwm_abi.zig"),
        .target = target,
        .optimize = optimize,
    });
    const pwm_test_module = b.createModule(.{
        .root_source_file = b.path("tests/pwm_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    pwm_test_module.addImport("abi", pwm_abi_module);
    const pwm_tests = b.addTest(.{ .root_module = pwm_test_module });

    // fw_os.h has no Zig implementation behind it; importing it here is what
    // makes a compiler read the port contract and its static_asserts.
    const os_contract_test_module = b.createModule(.{
        .root_source_file = b.path("tests/os_contract_test.zig"),
        .target = target,
        .optimize = optimize,
    });
    // libc's stdint.h, not the bare compiler one: its UINT32_MAX translates.
    os_contract_test_module.link_libc = true;
    os_contract_test_module.addIncludePath(b.path("inc"));
    os_contract_test_module.addIncludePath(b.path("../ra8_core/inc"));
    const os_contract_tests = b.addTest(.{ .root_module = os_contract_test_module });

    const run_path_tests = b.addRunArtifact(path_tests);
    const run_internal_tests = b.addRunArtifact(internal_tests);
    const run_abi_tests = b.addRunArtifact(abi_tests);
    const run_clock_tests = b.addRunArtifact(clock_tests);
    const run_timer_tests = b.addRunArtifact(timer_tests);
    const run_pwm_tests = b.addRunArtifact(pwm_tests);
    const run_os_contract_tests = b.addRunArtifact(os_contract_tests);
    const test_step = b.step("test", "Run Zig fw_if_fs tests");
    test_step.dependOn(&run_internal_tests.step);
    test_step.dependOn(&run_path_tests.step);
    test_step.dependOn(&run_abi_tests.step);
    test_step.dependOn(&run_clock_tests.step);
    test_step.dependOn(&run_timer_tests.step);
    test_step.dependOn(&run_pwm_tests.step);
    test_step.dependOn(&run_os_contract_tests.step);
}
