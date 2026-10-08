//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build graph for `if_ra8_gpt`, the RA8 GPT32 adapters behind the neutral
//! `fw_if_timer` and `fw_if_pwm` ports.
//!
//! Both adapters share one channel-ownership table (`internal/claim.zig`), the
//! only part that runs on the host without a GPT block, so it is the part with
//! a Zig test here. The ops reach `ra8_gpt_*` and `fw_timer_bind` /
//! `fw_pwm_bind` as externs resolved at the final link, and stay covered by
//! the untouched C suites `test_fw_if_timer_ra8` and `test_fw_if_pwm_ra8`.

const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const err = b.createModule(.{ .root_source_file = b.path("src/err.zig"), .target = target, .optimize = optimize });
    const hal = b.createModule(.{ .root_source_file = b.path("src/gpt_hal.zig"), .target = target, .optimize = optimize });
    const claim = b.createModule(.{ .root_source_file = b.path("src/internal/claim.zig"), .target = target, .optimize = optimize });
    claim.addImport("err", err);

    const timer_abi = b.createModule(.{ .root_source_file = b.path("src/timer_ra8_abi.zig"), .target = target, .optimize = optimize });
    const pwm_abi = b.createModule(.{ .root_source_file = b.path("src/pwm_ra8_abi.zig"), .target = target, .optimize = optimize });
    for ([_]*std.Build.Module{ timer_abi, pwm_abi }) |abi| {
        abi.addImport("err", err);
        abi.addImport("claim", claim);
        abi.addImport("gpt_hal", hal);
    }

    const root_module = b.createModule(.{ .root_source_file = b.path("src/root.zig"), .target = target, .optimize = optimize });
    root_module.addImport("timer_ra8_abi", timer_abi);
    root_module.addImport("pwm_ra8_abi", pwm_abi);

    const test_step = b.step("test", "Run Zig if_ra8_gpt tests");
    const claim_test = b.createModule(.{ .root_source_file = b.path("tests/claim_test.zig"), .target = target, .optimize = optimize });
    claim_test.addImport("claim", claim);
    test_step.dependOn(&b.addRunArtifact(b.addTest(.{ .root_module = claim_test })).step);

    const library = b.addLibrary(.{ .name = "if_ra8_gpt", .linkage = .static, .root_module = root_module });
    // The host C test executables are linked by the system toolchain rather
    // than by `zig cc`, so nothing else on that link line provides Zig's
    // runtime helpers.
    // A cortex-m image links with -lgcc -lm; compiler_rt's libm there is
    // soft-float and would shadow newlib's hard-float one (RA8FW-943).
    library.bundle_compiler_rt = library.root_module.resolved_target.?.result.os.tag != .freestanding;
    b.installArtifact(library);
}
