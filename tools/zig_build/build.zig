//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Build helpers shared by the Zig host applications. Host apps depend on this
//! package by path and `@import("ra8_zig_build")` inside their own `build.zig`,
//! so the target-selection rule lives in exactly one place and is unit tested.
//!
//! `zig build test` here runs those unit tests on any host, and
//! `zig build explain-host-target` prints the decision this package made for
//! the machine it is running on.

const std = @import("std");
const builtin = @import("builtin");

pub const macos_host = @import("macos_host.zig");

/// How the macOS libSystem stub is chosen. `auto` probes the host SDK (see
/// `macos_host.decide`); the other two are escape hatches for a host whose SDK
/// the probe reads wrongly.
pub const MacosLibSystem = enum { auto, sdk, bundled };

/// Everything the #899 rule worked out about this host, kept together so the
/// choice and the evidence for it can be printed as one story.
pub const HostTarget = struct {
    forced: MacosLibSystem,
    decision: macos_host.Decision,
    probe: macos_host.SdkProbe,
    host_macos_version: ?std.SemanticVersion,

    pub fn query(self: HostTarget) std.Target.Query {
        return self.decision.choice.query(self.host_macos_version);
    }
};

// `b.option` panics if the same option name is declared twice, so the whole
// decision is computed once per build graph and reused. That is what lets a
// build root wire `hostDefaultTargetQuery` into `standardTargetOptions` and
// still ask `hostTarget` for the reasoning afterwards.
var cached_owner: ?*std.Build = null;
var cached_host_target: HostTarget = undefined;

/// The #899 decision for this host, with the evidence behind it.
pub fn hostTarget(b: *std.Build) HostTarget {
    if (cached_owner) |owner| {
        if (owner == b) return cached_host_target;
    }

    const forced = b.option(
        MacosLibSystem,
        "macos-libsystem",
        "Which libSystem stub a native macOS host build links against (default: auto)",
    ) orelse .auto;

    const probe = probeHostSdk(b.allocator);
    const decision: macos_host.Decision = switch (forced) {
        .sdk => .{ .choice = .native, .reason = .sdk_declares_target },
        .bundled => .{ .choice = .pinned_macos_arm64, .reason = .sdk_omits_target },
        .auto => macos_host.decide(builtin.cpu.arch, builtin.os.tag, probe),
    };

    cached_host_target = .{
        .forced = forced,
        .decision = decision,
        .probe = probe,
        .host_macos_version = hostMacosVersion(),
    };
    cached_owner = b;
    return cached_host_target;
}

/// The default target query for a host application, as passed to
/// `b.standardTargetOptions(.{ .default_target = ... })`.
///
/// Off macOS this is the plain native query and nothing else happens. On an
/// arm64 Mac it may resolve to an explicit `aarch64-macos` query so that Zig
/// links its own `libSystem.tbd` instead of the Command Line Tools stub that
/// omits `arm64-macos` (#899). That pinned query carries the host's own macOS
/// version, so it keeps the deployment target a native build would have used.
/// `-Dtarget=...` still overrides it, and `-Dmacos-libsystem=sdk` forces the old
/// native behaviour back.
pub fn hostDefaultTargetQuery(b: *std.Build) std.Target.Query {
    return hostTarget(b).query();
}

/// The macOS version this build is running on, or null off macOS.
///
/// The build runner is compiled for the native target, so Zig's own host
/// detection has already read the running OS version and written it into
/// `builtin.os.version_range`; a native target sets the minimum and the maximum
/// to that one version. Reusing it keeps the pinned query in step with the
/// machine instead of falling back to Zig's default macOS range, and costs no
/// subprocess.
pub fn hostMacosVersion() ?std.SemanticVersion {
    if (builtin.os.tag != .macos) return null;
    return switch (builtin.os.version_range) {
        .semver => |range| range.min,
        else => null,
    };
}

/// Read the host SDK's `libSystem.tbd`, when there is one to read. Every failure
/// is a partial probe, and `macos_host.decide` turns each one into its own named
/// reason: "no SDK at all" and "an SDK whose stub I could not read" both pin the
/// target, but they are different things to have found.
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

    const tbd = std.fs.cwd().readFileAlloc(allocator, tbd_path, 4 * 1024 * 1024) catch
        return .{ .sdk_path = sdk_path, .libsystem_tbd_path = tbd_path };
    return .{ .sdk_path = sdk_path, .libsystem_tbd_path = tbd_path, .libsystem_tbd = tbd };
}

/// The host-target decision as a short report, for a build log or a gate.
///
/// This is the one place the answer is spelled out for a human: which machine
/// was read, which stub was read on it, what that stub said, and what the build
/// therefore targets. A gate that only prints the stub's `targets:` line cannot
/// distinguish "the stub omits us" from "there was no stub to read", and those
/// need different fixes.
pub fn describeHostTarget(b: *std.Build, host: HostTarget) []const u8 {
    var text: std.ArrayListUnmanaged(u8) = .empty;
    const out = text.writer(b.allocator);

    out.print("ra8 host target (#899)\n", .{}) catch @panic("OOM");
    out.print("  host:      {s}-{s}\n", .{ @tagName(builtin.cpu.arch), @tagName(builtin.os.tag) }) catch @panic("OOM");
    if (host.host_macos_version) |version| {
        out.print("  macos:     {d}.{d}.{d}\n", .{ version.major, version.minor, version.patch }) catch @panic("OOM");
    }
    out.print("  selection: -Dmacos-libsystem={s}\n", .{@tagName(host.forced)}) catch @panic("OOM");
    out.print("  sdk:       {s}\n", .{host.probe.sdk_path orelse "(none located)"}) catch @panic("OOM");
    out.print("  stub:      {s}\n", .{host.probe.libsystem_tbd_path orelse "(none read)"}) catch @panic("OOM");
    if (host.forced == .auto) {
        out.print("  finding:   {s}\n", .{host.decision.reason.explain()}) catch @panic("OOM");
    } else {
        out.print("  finding:   forced by -Dmacos-libsystem={s}; the SDK probe was not consulted\n", .{@tagName(host.forced)}) catch @panic("OOM");
    }

    switch (host.decision.choice) {
        .native => out.print("  decision:  native target, linking whatever stub the host resolves\n", .{}) catch @panic("OOM"),
        .pinned_macos_arm64 => {
            out.print("  decision:  pinned aarch64-macos, linking Zig's bundled libSystem stub\n", .{}) catch @panic("OOM");
            if (macos_host.pinnedOsVersion(host.host_macos_version)) |version| {
                out.print("  deployment target: {d}.{d}.{d} (carried from the host)\n", .{ version.major, version.minor, version.patch }) catch @panic("OOM");
            } else {
                out.print("  deployment target: Zig's default macOS range (host version unknown)\n", .{}) catch @panic("OOM");
            }
        },
    }
    return text.items;
}

/// A step that prints `describeHostTarget`. Printing at configure time would
/// put this in front of every `zig build` on every host; a step prints it when
/// someone actually asks.
pub fn addExplainHostTargetStep(b: *std.Build, host: HostTarget) *std.Build.Step {
    const explain = b.allocator.create(ExplainHostTarget) catch @panic("OOM");
    explain.* = .{
        .step = std.Build.Step.init(.{
            .id = .custom,
            .name = "explain host target",
            .owner = b,
            .makeFn = ExplainHostTarget.make,
        }),
        .text = describeHostTarget(b, host),
    };
    return &explain.step;
}

const ExplainHostTarget = struct {
    step: std.Build.Step,
    text: []const u8,

    fn make(step: *std.Build.Step, options: std.Build.Step.MakeOptions) anyerror!void {
        _ = options;
        const self: *ExplainHostTarget = @fieldParentPtr("step", step);
        std.debug.print("{s}", .{self.text});
    }
};

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

    const explain_step = b.step(
        "explain-host-target",
        "Print how the macOS host target was chosen on this machine (#899)",
    );
    explain_step.dependOn(addExplainHostTargetStep(b, hostTarget(b)));
}
