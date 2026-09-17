//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build helpers shared by the Zig host applications. Host apps depend on this
//! package by path and `@import("ra8_zig_build")` inside their own `build.zig`,
//! so the target-selection rule lives in exactly one place and is unit tested.
//!
//! `zig build test` here runs those unit tests on any host.

const std = @import("std");
const builtin = @import("builtin");

pub const macos_host = @import("macos_host.zig");

/// How the macOS libSystem stub is chosen. `auto` probes the host SDK (see
/// `macos_host.decide`); the other two are escape hatches for a host whose SDK
/// the probe reads wrongly.
pub const MacosLibSystem = enum { auto, sdk, bundled };

/// The default target query for a host application, as passed to
/// `b.standardTargetOptions(.{ .default_target = ... })`.
///
/// Off macOS this is the plain native query and nothing else happens. On an
/// arm64 Mac it may resolve to an explicit `aarch64-macos` query so that Zig
/// links its own `libSystem.tbd` instead of the Command Line Tools stub that
/// omits `arm64-macos` (#899). `-Dtarget=...` still overrides it, and
/// `-Dmacos-libsystem=sdk` forces the old native behaviour back.
pub fn hostDefaultTargetQuery(b: *std.Build) std.Target.Query {
    const forced = b.option(
        MacosLibSystem,
        "macos-libsystem",
        "Which libSystem stub a native macOS host build links against (default: auto)",
    ) orelse .auto;

    const choice: macos_host.Choice = switch (forced) {
        .sdk => .native,
        .bundled => .pinned_macos_arm64,
        .auto => macos_host.decide(
            builtin.cpu.arch,
            builtin.os.tag,
            probeHostSdk(b.allocator),
        ),
    };
    return choice.query();
}

/// Read the host SDK's `libSystem.tbd`, when there is one to read. Every failure
/// is an empty probe: `macos_host.decide` treats "could not tell" as "do not
/// trust the SDK", which is the safe direction for these libc-only host tools.
pub fn probeHostSdk(allocator: std.mem.Allocator) macos_host.SdkProbe {
    if (builtin.os.tag != .macos) return .{};

    const sdk_run = std.process.Child.run(.{
        .allocator = allocator,
        .argv = &.{ "xcrun", "--show-sdk-path" },
    }) catch return .{};
    defer allocator.free(sdk_run.stdout);
    defer allocator.free(sdk_run.stderr);
    if (sdk_run.term != .Exited or sdk_run.term.Exited != 0) return .{};

    const sdk_path = allocator.dupe(u8, std.mem.trim(u8, sdk_run.stdout, " \t\r\n")) catch return .{};
    if (sdk_path.len == 0) return .{};

    const tbd_path = std.fs.path.join(allocator, &.{ sdk_path, "usr", "lib", "libSystem.tbd" }) catch
        return .{ .sdk_path = sdk_path };
    defer allocator.free(tbd_path);

    const tbd = std.fs.cwd().readFileAlloc(allocator, tbd_path, 4 * 1024 * 1024) catch
        return .{ .sdk_path = sdk_path };
    return .{ .sdk_path = sdk_path, .libsystem_tbd = tbd };
}

/// Keep a cross-configured build root's test step honest.
///
/// The host apps default to an explicit `aarch64-macos` query on Apple silicon
/// (#899), and that same query is how a Linux checkout exercises the Mach-O
/// link path. Compiling and linking works from anywhere; running the result
/// does not, and a plain run step turns that into a hard failure ("the host
/// system (x86_64-linux) is unable to execute binaries from the target
/// (aarch64-macos)"), which makes `zig build test -Dtarget=aarch64-macos`
/// unusable as a check.
///
/// So `run` is marked skippable on a foreign host, and `test_step` also depends
/// on the compile directly: when the binary cannot run, it is still built and
/// linked, and Zig's build summary reports the run as skipped rather than
/// passed. On a real arm64 Mac the target is native and the tests run normally.
///
/// Each build root still creates its own run artifact and depends on it, so the
/// wiring stays visible where `scripts/checks/check_zig.py` reads it.
pub fn allowForeignHostTests(
    test_step: *std.Build.Step,
    tests: *std.Build.Step.Compile,
    run: *std.Build.Step.Run,
) void {
    run.skip_foreign_checks = true;
    // Linking is the property #899 is about, so it must happen even on a host
    // that cannot execute the result.
    test_step.dependOn(&tests.step);
}

pub fn build(b: *std.Build) void {
    // This package's own test graph is a host build like any other, so it takes
    // the same macOS host target rule it hands to the apps (#899).
    const target = b.standardTargetOptions(.{ .default_target = hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});

    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("macos_host", b.createModule(.{
        .root_source_file = b.path("macos_host.zig"),
        .target = target,
        .optimize = optimize,
    }));
    const tests = b.addTest(.{ .root_module = test_module });

    const test_step = b.step("test", "Run host-target selection tests");
    const run_tests = b.addRunArtifact(tests);
    test_step.dependOn(&run_tests.step);
    allowForeignHostTests(test_step, tests, run_tests);
}
