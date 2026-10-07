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
pub const ar = @import("ar.zig");
pub const macho = ar.macho;

/// How the macOS libSystem stub is chosen. `auto` probes the host SDK (see
/// `macos_host.decide`); the other two are escape hatches for a host whose SDK
/// the probe reads wrongly. The enum lives beside the rule it selects, so the
/// unit tests can reason about a forced selection without a build graph.
pub const MacosLibSystem = macos_host.Selection;

/// Everything the RA8FW-330 rule worked out about this host, kept together so the
/// choice and the evidence for it can be printed as one story.
pub const HostTarget = struct {
    forced: MacosLibSystem,
    /// What this build does, and what the probe read off the machine. Under
    /// `-Dmacos-libsystem=` those are two different things, and both are worth
    /// printing.
    resolution: macos_host.Resolution,
    probe: macos_host.SdkProbe,
    host_macos_version: ?std.SemanticVersion,

    /// The decision the build acts on.
    pub fn decision(self: HostTarget) macos_host.Decision {
        return self.resolution.effective;
    }

    pub fn query(self: HostTarget) std.Target.Query {
        return self.resolution.effective.choice.query(self.host_macos_version);
    }
};

// `b.option` panics if the same option name is declared twice, so the whole
// decision is computed once per build graph and reused. That is what lets a
// build root wire `hostDefaultTargetQuery` into `standardTargetOptions` and
// still ask `hostTarget` for the reasoning afterwards.
var cached_owner: ?*std.Build = null;
var cached_host_target: HostTarget = undefined;

/// The RA8FW-330 decision for this host, with the evidence behind it.
pub fn hostTarget(b: *std.Build) HostTarget {
    if (cached_owner) |owner| {
        if (owner == b) return cached_host_target;
    }

    const forced = b.option(
        MacosLibSystem,
        "macos-libsystem",
        "Which libSystem stub a native macOS host build links against (default: auto)",
    ) orelse .auto;

    // The probe runs whatever the selection is. It costs one `xcrun` call, and
    // a forced leg is precisely where its finding matters: the gate's
    // `-Dmacos-libsystem=sdk` run exists to record what the SDK stub does on
    // that runner, which is unreadable if the forced choice is reported as the
    // probe's own conclusion.
    const probe = probeHostSdk(b.allocator, b.graph.io);

    cached_host_target = .{
        .forced = forced,
        .resolution = macos_host.resolve(forced, builtin.cpu.arch, builtin.os.tag, probe),
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
/// omits `arm64-macos` (RA8FW-330). That pinned query carries the host's own macOS
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
    return builtin.os.version_range.semver.min;
}

/// The largest `libSystem.tbd` the probe reads, inclusive.
const max_tbd_bytes = 4 * 1024 * 1024;

/// Read the host SDK's `libSystem.tbd`, when there is one to read. Every failure
/// is a partial probe, and `macos_host.decide` turns each one into its own named
/// reason: "no SDK at all" and "an SDK whose stub I could not read" both pin the
/// target, but they are different things to have found.
pub fn probeHostSdk(allocator: std.mem.Allocator, io: std.Io) macos_host.SdkProbe {
    if (builtin.os.tag != .macos) return .{};

    // Name the SDK, exactly as the compiler does. Zig resolves its own sysroot
    // with `xcrun --sdk macosx --show-sdk-path` for a macOS target
    // (`std.zig.system.darwin.getSdk`). The bare `xcrun --show-sdk-path` this
    // probe used to run is a different question: it reports the *active* SDK,
    // which `SDKROOT` in the environment redirects. A shell carrying
    // `SDKROOT=iphoneos` therefore had the probe read an iOS `libSystem.tbd`,
    // whose targets are `arm64-ios` and friends, and report `arm64-macos`
    // absent -- RA8FW-330's own signature, about an SDK no macOS link would use.
    const queried_sdk = macos_host.host_sdk_name;
    const sdk_run = std.process.run(allocator, io, .{
        .argv = &.{ "xcrun", "--sdk", queried_sdk, "--show-sdk-path" },
    }) catch return .{ .queried_sdk = queried_sdk };
    defer allocator.free(sdk_run.stdout);
    defer allocator.free(sdk_run.stderr);
    if (!sdk_run.term.success()) return .{ .queried_sdk = queried_sdk };

    const sdk_path = allocator.dupe(u8, std.mem.trim(u8, sdk_run.stdout, " \t\r\n")) catch
        return .{ .queried_sdk = queried_sdk };
    if (sdk_path.len == 0) return .{ .queried_sdk = queried_sdk };

    const tbd_path = std.fs.path.join(allocator, &.{ sdk_path, "usr", "lib", "libSystem.tbd" }) catch
        return .{ .sdk_path = sdk_path, .queried_sdk = queried_sdk };

    const tbd = std.Io.Dir.cwd().readFileAlloc(io, tbd_path, allocator, .limited(max_tbd_bytes + 1)) catch
        return .{ .sdk_path = sdk_path, .libsystem_tbd_path = tbd_path, .queried_sdk = queried_sdk };
    return .{
        .sdk_path = sdk_path,
        .libsystem_tbd_path = tbd_path,
        .libsystem_tbd = tbd,
        .queried_sdk = queried_sdk,
    };
}

/// A step that refuses a toolchain whose bundled `libSystem` stub cannot link
/// the pinned target (RA8FW-330).
///
/// It runs on every host on purpose. The failure it guards against arrives
/// with a Zig upgrade, not with a Mac, and a Linux checkout cross-building
/// `-Dtarget=aarch64-macos` links the very same file: catching it here means
/// catching it on the machine that does the upgrade, rather than on the next
/// nightly run of the one Mac in the CI suite.
///
/// The stub is read by `check` when the step runs, not at configure time:
/// Zig 0.17's build graph does not expose the Zig lib directory to build.zig,
/// so the tool receives it as `LazyPath.zig_lib` instead.
pub fn addVerifyBundledStubStep(b: *std.Build, check: *std.Build.Step.Compile) *std.Build.Step {
    const run = b.addRunArtifact(check);
    run.setName("verify bundled libSystem stub");
    run.has_side_effects = true;
    run.addArg("bundled-stub");
    run.addDirectoryArg(.zig_lib);
    return &run.step;
}

/// One phrase naming a `Choice`, for a sentence about a road not taken.
fn describeChoice(choice: macos_host.Choice) []const u8 {
    return switch (choice) {
        .native => "the native target",
        .pinned_macos_arm64 => "the pinned aarch64-macos target",
    };
}

/// `describeHostTarget`'s report, split where `check explain` inserts the
/// bundled-stub lines it reads when the step runs.
pub const HostReport = struct {
    head: []const u8,
    tail: []const u8,
};

/// The host-target decision as a short report, for a build log or a gate.
///
/// This is the one place the answer is spelled out for a human: which machine
/// was read, which stub was read on it, what that stub said, and what the build
/// therefore targets. A gate that only prints the stub's `targets:` line cannot
/// distinguish "the stub omits us" from "there was no stub to read", and those
/// need different fixes.
pub fn describeHostTarget(b: *std.Build, host: HostTarget) HostReport {
    var text: std.Io.Writer.Allocating = .init(b.allocator);
    const out = &text.writer;

    out.print("ra8 host target (RA8FW-330)\n", .{}) catch @panic("OOM");
    out.print("  host:      {s}-{s}\n", .{ @tagName(builtin.cpu.arch), @tagName(builtin.os.tag) }) catch @panic("OOM");
    if (host.host_macos_version) |version| {
        out.print("  macos:     {d}.{d}.{d}\n", .{ version.major, version.minor, version.patch }) catch @panic("OOM");
    }
    out.print("  selection: -Dmacos-libsystem={s}\n", .{@tagName(host.forced)}) catch @panic("OOM");
    out.print("  sdk query: {s}\n", .{
        if (host.probe.queried_sdk) |name| name else "(xcrun not run on this host)",
    }) catch @panic("OOM");
    out.print("  sdk:       {s}\n", .{host.probe.sdk_path orelse "(none located)"}) catch @panic("OOM");
    out.print("  stub:      {s}\n", .{host.probe.libsystem_tbd_path orelse "(none read)"}) catch @panic("OOM");
    // The SDK stub is what RA8FW-330 reports; Zig's own stub is what the fix
    // links instead. A report that names only the first cannot say whether
    // the workaround still has anything to stand on.
    // `check explain` prints those two lines between head and tail.
    const head_len = text.written().len;

    // The finding is always what the machine said. A force changes what the
    // build does, not what the SDK stub contains, and printing the forced
    // choice as the finding is how an informational leg stops being evidence.
    out.print("  finding:   {s}\n", .{host.resolution.observed.reason.explain()}) catch @panic("OOM");
    if (host.forced != .auto) {
        out.print("  override:  {s}\n", .{host.resolution.effective.reason.explain()}) catch @panic("OOM");
        if (host.resolution.overridesProbe()) {
            out.print("  note:      this overrides the probe, which would have chosen {s}\n", .{
                describeChoice(host.resolution.observed.choice),
            }) catch @panic("OOM");
        } else {
            out.print("  note:      this matches what the probe would have chosen anyway\n", .{}) catch @panic("OOM");
        }
    }

    switch (host.decision().choice) {
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
    const report = text.written();
    return .{ .head = report[0..head_len], .tail = report[head_len..] };
}

/// A step that prints `describeHostTarget`. Printing at configure time would
/// put this in front of every `zig build` on every host; a step prints it when
/// someone actually asks.
pub fn addExplainHostTargetStep(
    b: *std.Build,
    check: *std.Build.Step.Compile,
    host: HostTarget,
) *std.Build.Step {
    const report = describeHostTarget(b, host);
    const run = b.addRunArtifact(check);
    run.setName("explain host target");
    run.has_side_effects = true;
    run.addArg("explain");
    run.addDirectoryArg(.zig_lib);
    run.addArgs(&.{ report.head, report.tail });
    return &run.step;
}

/// Wire a test binary into `test_step` so a cross-configured build root still
/// proves what it can.
///
/// The host apps default to an explicit `aarch64-macos` query on Apple silicon
/// (RA8FW-330), and that same query is how a Linux checkout exercises the Mach-O
/// link path. Compiling and linking works from anywhere; running the result
/// does not, and a plain `b.addRunArtifact` turns that into a hard failure
/// ("the host system (x86_64-linux) is unable to execute binaries from the
/// target (aarch64-macos)"), which makes `zig build test -Dtarget=aarch64-macos`
/// unusable as a check.
///
/// So the run is marked skippable only when the target really is foreign to the
/// build host, and `test_step` also depends on the compile directly: the binary
/// is still built and linked, and Zig's build summary reports the run as
/// skipped rather than passed.
///
/// The excuse is SCOPED to a target this host cannot execute rather than
/// granted unconditionally, which is the whole point of this function's
/// existence. `skip_foreign_checks = true` on every host would also forgive a
/// Mac on which the host tests cannot run: the run would be dropped, `zig build
/// test` would exit zero, and the macOS gate's verdict -- which is that these
/// tests RUN natively on Apple silicon -- would rest on tests that never
/// executed, with nothing in the exit status to say so. On an arm64 Mac the
/// pinned `aarch64-macos` target is the host, so the excuse does not apply and
/// a run that cannot happen fails loudly instead.
pub fn addHostTestRun(
    b: *std.Build,
    test_step: *std.Build.Step,
    tests: *std.Build.Step.Compile,
) *std.Build.Step.Run {
    const run = b.addRunArtifact(tests);
    const resolved = tests.rootModuleTarget();
    run.skip_foreign_checks = !macos_host.targetRunsOnBuildHost(
        resolved.cpu.arch,
        resolved.os.tag,
        builtin.cpu.arch,
        builtin.os.tag,
    );
    // Linking is the property RA8FW-330 is about, so it must happen even on a host
    // that cannot execute the result.
    test_step.dependOn(&tests.step);
    test_step.dependOn(&run.step);
    return run;
}

/// Check what a host build actually produced, rather than trusting that a
/// zero exit means the right thing was linked (RA8FW-330).
///
/// The rule this package implements is a claim about the emitted Mach-O: on an
/// arm64 Mac it must be a native arm64 image, stamped with the deployment
/// target the build was configured for, linked against the system `libSystem`.
/// `zig build` exiting zero says none of that. A build that silently took
/// Zig's default macOS floor instead of the host's version still exits zero,
/// and so would one that came out for the wrong architecture.
///
/// The expectations are read off the resolved target at configure time, so the
/// step checks the binary against what this very build asked for. Off macOS it
/// says so and passes: there is no Mach-O to read, and a cross-build from Linux
/// should still be able to run the step without a special case at the call
/// site.
pub fn addVerifyHostArtifactStep(
    b: *std.Build,
    compile: *std.Build.Step.Compile,
) *std.Build.Step {
    const resolved = compile.rootModuleTarget();
    const run = b.addRunArtifact(checkTool(b));
    run.setName(b.fmt("verify host artifact {s}", .{compile.name}));
    run.has_side_effects = true;
    run.addArg("host-artifact");
    run.addFileArg(compile.getEmittedBin());
    run.addArgs(&.{ compile.name, @tagName(resolved.cpu.arch), @tagName(resolved.os.tag) });
    // `VersionRange` is an untagged union, so the os tag is what says which
    // member is live; for macOS that is always the semver range.
    run.addArg(switch (resolved.os.tag) {
        .macos => b.fmt("{f}", .{resolved.os.version_range.semver.min}),
        else => "-",
    });
    return &run.step;
}

/// Refuse a static archive that this build's target cannot link, and say why
/// (RA8FW-330).
///
/// The three host roots that consume a Rust archive get it from a separate
/// `cargo` invocation. `cargo` with no `--target` builds for the machine it
/// runs on, so the archive matches the Zig target only while nobody pins a
/// target and nobody reuses a target directory that a different host filled
/// in. When they disagree the link fails inside the linker, naming a symbol or
/// a "file was built for a different architecture" line rather than the
/// mismatch itself, which is the reason those roots are still outside the
/// macOS gate.
///
/// This step reads the archive before the link is attempted and fails with the
/// target it was configured for, what the archive actually holds, and the
/// option that fixes it. Expectations come off the resolved target at
/// configure time, so it checks the archive against what this very build asked
/// for rather than a hardcoded answer.
pub fn addRequireArchiveForTargetStep(
    b: *std.Build,
    compile: *std.Build.Step.Compile,
    archive_path: []const u8,
    option_hint: []const u8,
) *std.Build.Step {
    const resolved = compile.rootModuleTarget();
    const run = b.addRunArtifact(checkTool(b));
    run.setName(b.fmt("require archive for {s}", .{compile.name}));
    run.has_side_effects = true;
    run.addArg("archive");
    // The archive comes from a separate cargo run, so it is a path the build
    // reads, not a build output; the tool reports a missing one.
    run.addArgs(&.{
        archive_path,
        compile.name,
        @tagName(resolved.cpu.arch),
        @tagName(resolved.os.tag),
        option_hint,
    });
    return &run.step;
}

/// The check tool as built by this package, for a consumer that imports
/// `ra8_zig_build` as a dependency.
fn checkTool(b: *std.Build) *std.Build.Step.Compile {
    return b.dependencyFromBuildZig(@This(), .{}).artifact("ra8-build-check");
}

/// The host tool the check steps run (`check.zig`). It always targets the
/// build host: it runs during the build, whatever `-Dtarget=` says.
fn addCheckTool(b: *std.Build) *std.Build.Step.Compile {
    const host_rule = b.createModule(.{
        .root_source_file = b.path("macos_host.zig"),
        .target = b.graph.host,
    });
    const root = b.createModule(.{
        .root_source_file = b.path("check.zig"),
        .target = b.graph.host,
    });
    root.addImport("macos_host", host_rule);
    root.addImport("ar", b.createModule(.{
        .root_source_file = b.path("ar.zig"),
        .target = b.graph.host,
    }));
    return b.addExecutable(.{ .name = "ra8-build-check", .root_module = root });
}

pub fn build(b: *std.Build) void {
    // This package's own test graph is a host build like any other, so it takes
    // the same macOS host target rule it hands to the apps (RA8FW-330).
    const target = b.standardTargetOptions(.{ .default_target = hostDefaultTargetQuery(b) });
    const optimize = b.standardOptimizeOption(.{});

    const macos_host_module = b.createModule(.{
        .root_source_file = b.path("macos_host.zig"),
        .target = target,
        .optimize = optimize,
    });
    const ar_module = b.createModule(.{
        .root_source_file = b.path("ar.zig"),
        .target = target,
        .optimize = optimize,
    });
    const test_module = b.createModule(.{
        .root_source_file = b.path("tests/root.zig"),
        .target = target,
        .optimize = optimize,
    });
    test_module.addImport("macos_host", macos_host_module);
    // `macho.zig` is reached through the `ar` module, not rooted separately:
    // a file can belong to only one module in a compilation, and the archive
    // reader imports it.
    test_module.addImport("ar", ar_module);

    const tests = b.addTest(.{ .root_module = test_module });
    const test_step = b.step("test", "Run host-target selection tests");
    _ = addHostTestRun(b, test_step, tests);
    const explain_step = b.step(
        "explain-host-target",
        "Print how the macOS host target was chosen on this machine (RA8FW-330)",
    );
    const check = addCheckTool(b);
    // Installed so a consumer's `checkTool` finds it by name.
    b.installArtifact(check);
    explain_step.dependOn(addExplainHostTargetStep(b, check, hostTarget(b)));

    // The pinned target is only a workaround while Zig's own libSystem stub
    // declares arm64-macos, and that is a property of the toolchain, not of
    // the machine, so this runs on every host rather than only on a Mac.
    const bundled_step = b.step(
        "verify-bundled-stub",
        "Check that this Zig's own libSystem stub can link the pinned host target (RA8FW-330)",
    );
    bundled_step.dependOn(addVerifyBundledStubStep(b, check));
}
