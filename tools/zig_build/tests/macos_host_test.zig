//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the #899 host-target selection rule.

const std = @import("std");
const macos_host = @import("macos_host");

const testing = std.testing;
const Choice = macos_host.Choice;
const Reason = macos_host.Reason;
const TbdVerdict = macos_host.TbdVerdict;
const archsFieldDeclares = macos_host.archsFieldDeclares;
const classifyTbd = macos_host.classifyTbd;
const decide = macos_host.decide;
const pinnedOsVersion = macos_host.pinnedOsVersion;
const required_target = macos_host.required_target;
const targetsFieldDeclares = macos_host.targetsFieldDeclares;
const targetsFieldMentionsOs = macos_host.targetsFieldMentionsOs;
const platformFieldDeclares = macos_host.platformFieldDeclares;
const archsFieldContains = macos_host.archsFieldContains;
const tbdDeclaresTarget = macos_host.tbdDeclaresTarget;
const Selection = macos_host.Selection;
const resolve = macos_host.resolve;
const targetRunsOnBuildHost = macos_host.targetRunsOnBuildHost;

const broken_clt_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ x86_64-macos, x86_64-maccatalyst, arm64e-macos, arm64e-maccatalyst ]
    \\uuids:
    \\  - target: arm64e-macos
    \\    value: 00000000-0000-0000-0000-000000000000
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const healthy_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ x86_64-macos, arm64-macos, arm64e-macos ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const block_list_tbd =
    \\--- !tapi-tbd
    \\targets:
    \\  - x86_64-macos
    \\  - arm64-macos
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

const wrapped_flow_tbd =
    \\--- !tapi-tbd
    \\targets: [ x86_64-macos, x86_64-maccatalyst,
    \\           arm64-macos, arm64e-macos ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

/// A TAPI v3 stub: `archs:` plus a separate `platform:`, no triples anywhere.
const v3_arm64_tbd =
    \\--- !tapi-tbd-v3
    \\archs:           [ i386, x86_64, arm64, arm64e ]
    \\platform:        macosx
    \\install-name:    '/usr/lib/libSystem.B.dylib'
    \\exports:
    \\  - archs:       [ arm64 ]
    \\    symbols:     [ _abort ]
    \\
;

const v3_intel_only_tbd =
    \\--- !tapi-tbd-v3
    \\archs:           [ i386, x86_64 ]
    \\platform:        macosx
    \\install-name:    '/usr/lib/libSystem.B.dylib'
    \\
;

const v3_ios_tbd =
    \\--- !tapi-tbd-v3
    \\archs:           [ arm64, arm64e ]
    \\platform:        ios
    \\install-name:    '/usr/lib/libSystem.B.dylib'
    \\
;

/// A TAPI v4 iPhoneOS stub. This is the file the probe reads when `xcrun` is
/// asked for the *active* SDK in a shell carrying `SDKROOT=iphoneos`: every
/// target is an iOS one, so the macOS triple is absent for a reason that has
/// nothing to do with #899.
const v4_ios_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ arm64-ios, arm64e-ios, arm64-ios-simulator ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

/// A macOS stub that lists maccatalyst beside macos. `maccatalyst` must not be
/// read as a macOS target, or an SDK that only ever declared catalyst slices
/// would look like this host's own.
const v4_catalyst_only_tbd =
    \\--- !tapi-tbd
    \\tbd-version: 4
    \\targets: [ x86_64-maccatalyst, arm64-maccatalyst ]
    \\install-name: '/usr/lib/libSystem.B.dylib'
    \\
;

test "arm64e-macos does not answer for arm64-macos" {
    try testing.expect(!tbdDeclaresTarget(broken_clt_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(broken_clt_tbd, "arm64e-macos"));
    try testing.expect(tbdDeclaresTarget(broken_clt_tbd, "x86_64-macos"));
}

test "healthy, block-list, and wrapped flow spellings all declare arm64-macos" {
    try testing.expect(tbdDeclaresTarget(healthy_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(block_list_tbd, required_target));
    try testing.expect(tbdDeclaresTarget(wrapped_flow_tbd, required_target));
}

test "uuids entries never answer for the targets list" {
    const uuids_only =
        \\--- !tapi-tbd
        \\targets: [ arm64e-macos ]
        \\uuids:
        \\  - target: arm64-macos
        \\    value: 00000000-0000-0000-0000-000000000000
        \\
    ;
    try testing.expect(!tbdDeclaresTarget(uuids_only, required_target));
}

test "a tbd-v3 stub is read through archs plus platform, not targets" {
    // The v4 reader must report "no such field" rather than "absent", or the
    // v3 fallback never gets asked.
    try testing.expect(targetsFieldDeclares(v3_arm64_tbd, required_target) == null);

    try testing.expectEqual(TbdVerdict.declares, classifyTbd(v3_arm64_tbd, required_target));
    try testing.expectEqual(TbdVerdict.omits, classifyTbd(v3_intel_only_tbd, required_target));

    // Right arch, wrong platform: the two halves have to agree, and the
    // disagreement is named for what it is rather than folded into #899.
    try testing.expectEqual(TbdVerdict.foreign_platform, classifyTbd(v3_ios_tbd, required_target));
    try testing.expectEqual(@as(?bool, true), archsFieldDeclares(v3_ios_tbd, "arm64", "ios"));
}

test "an iOS stub is not read as a macOS SDK that omits us" {
    // The distinction this draws: both files lack arm64-macos, and only one of
    // them is #899. Reporting the iOS stub as `sdk_omits_target` sends the
    // reader after Apple's macOS stub when the fault is in which SDK was read.
    try testing.expectEqual(TbdVerdict.omits, classifyTbd(broken_clt_tbd, required_target));
    try testing.expectEqual(TbdVerdict.foreign_platform, classifyTbd(v4_ios_tbd, required_target));

    // Asking the iOS stub its own question still answers plainly.
    try testing.expectEqual(@as(?bool, true), targetsFieldDeclares(v4_ios_tbd, "arm64-ios"));
}

test "maccatalyst is not a macOS target" {
    try testing.expectEqual(TbdVerdict.foreign_platform, classifyTbd(v4_catalyst_only_tbd, required_target));
    try testing.expectEqual(@as(?bool, false), targetsFieldMentionsOs(v4_catalyst_only_tbd, "macos"));

    // The broken CLT stub carries maccatalyst too, but it also carries real
    // macos targets, so it stays #899 rather than being excused as foreign.
    try testing.expectEqual(@as(?bool, true), targetsFieldMentionsOs(broken_clt_tbd, "macos"));
}

test "the os question reads the targets list, and only the targets list" {
    try testing.expect(targetsFieldMentionsOs("--- !tapi-tbd\ninstall-name: '/usr/lib/libSystem.B.dylib'\n", "macos") == null);
    try testing.expectEqual(@as(?bool, true), targetsFieldMentionsOs(healthy_tbd, "macos"));
    try testing.expectEqual(@as(?bool, true), targetsFieldMentionsOs(block_list_tbd, "macos"));
    try testing.expectEqual(@as(?bool, true), targetsFieldMentionsOs(wrapped_flow_tbd, "macos"));

    // `macos` must not be found inside a longer OS token, nor without the
    // triple's separating dash in front of it.
    const macosx_only =
        \\--- !tapi-tbd
        \\targets: [ arm64-macosx ]
        \\
    ;
    try testing.expectEqual(@as(?bool, false), targetsFieldMentionsOs(macosx_only, "macos"));
    const bare_word =
        \\--- !tapi-tbd
        \\targets: [ macos ]
        \\
    ;
    try testing.expectEqual(@as(?bool, false), targetsFieldMentionsOs(bare_word, "macos"));
}

test "the v3 halves can be asked separately" {
    try testing.expectEqual(@as(?bool, true), archsFieldContains(v3_arm64_tbd, "arm64"));
    try testing.expectEqual(@as(?bool, false), archsFieldContains(v3_intel_only_tbd, "arm64"));
    try testing.expect(archsFieldContains(healthy_tbd, "arm64") == null);

    try testing.expectEqual(@as(?bool, true), platformFieldDeclares(v3_arm64_tbd, "macos"));
    try testing.expectEqual(@as(?bool, false), platformFieldDeclares(v3_ios_tbd, "macos"));
    try testing.expectEqual(@as(?bool, true), platformFieldDeclares(v3_ios_tbd, "ios"));
    try testing.expect(platformFieldDeclares(healthy_tbd, "macos") == null);

    // A v3 macOS stub that genuinely lacks arm64 is still #899, not foreign.
    try testing.expectEqual(TbdVerdict.omits, classifyTbd(v3_intel_only_tbd, required_target));
}

test "a foreign stub pins the target and says so in its own words" {
    const decision = decide(.aarch64, .macos, .{ .sdk_path = "/sdk", .libsystem_tbd = v4_ios_tbd });
    try testing.expectEqual(Choice.pinned_macos_arm64, decision.choice);
    try testing.expectEqual(Reason.sdk_stub_foreign_platform, decision.reason);

    // The two neighbouring findings stay distinct from it.
    try testing.expectEqual(
        Reason.sdk_omits_target,
        decide(.aarch64, .macos, .{ .sdk_path = "/sdk", .libsystem_tbd = broken_clt_tbd }).reason,
    );
    try testing.expectEqual(
        Reason.sdk_stub_unrecognized,
        decide(.aarch64, .macos, .{ .sdk_path = "/sdk", .libsystem_tbd = "--- !tapi-tbd\n" }).reason,
    );

    // And the explanation names the SDK the compiler would have asked for, so
    // a reader can check which one was actually read.
    try testing.expect(std.mem.indexOf(u8, Reason.sdk_stub_foreign_platform.explain(), macos_host.host_sdk_name) != null);
}

test "the host sdk name is the one zig itself asks xcrun for" {
    // std.zig.system.darwin.getSdk maps a .macos target to this sdk name and
    // runs `xcrun --sdk <name> --show-sdk-path`. The probe has to ask the same
    // question or it can read an SDK the link never uses.
    try testing.expectEqualStrings("macosx", macos_host.host_sdk_name);
}

test "a stub with no target list at all is unrecognised, not silently absent" {
    const no_list =
        \\--- !tapi-tbd
        \\tbd-version: 4
        \\install-name: '/usr/lib/libSystem.B.dylib'
        \\current-version: 1345.100.2
        \\
    ;
    try testing.expectEqual(TbdVerdict.unrecognized, classifyTbd(no_list, required_target));
    try testing.expect(targetsFieldDeclares(no_list, required_target) == null);
    try testing.expect(archsFieldDeclares(no_list, "arm64", "macos") == null);

    // The plain boolean question still answers "no" for it.
    try testing.expect(!tbdDeclaresTarget(no_list, required_target));
}

test "only an arm64 Mac with a broken SDK gets pinned" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .libsystem_tbd = broken_clt_tbd }).choice);
    try testing.expectEqual(Choice.native, decide(.aarch64, .macos, .{ .libsystem_tbd = healthy_tbd }).choice);
    try testing.expectEqual(Choice.native, decide(.x86_64, .macos, .{ .libsystem_tbd = broken_clt_tbd }).choice);
    try testing.expectEqual(Choice.native, decide(.aarch64, .linux, .{}).choice);
    try testing.expectEqual(Choice.native, decide(.x86_64, .linux, .{}).choice);

    // A v3 SDK that really does carry arm64 is left native, and says so.
    try testing.expectEqual(Choice.native, decide(.aarch64, .macos, .{ .libsystem_tbd = v3_arm64_tbd }).choice);
}

test "an unreadable SDK falls back to the bundled stub on arm64 macOS" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{}).choice);
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .sdk_path = "/nope" }).choice);
}

test "the three pinning states are reported as three different reasons" {
    // No SDK at all, an SDK whose stub could not be read, and a stub that was
    // read and does not list us: all pin, none of them is the same finding.
    try testing.expectEqual(Reason.sdk_not_probed, decide(.aarch64, .macos, .{}).reason);
    try testing.expectEqual(Reason.sdk_stub_unreadable, decide(.aarch64, .macos, .{ .sdk_path = "/nope" }).reason);
    try testing.expectEqual(Reason.sdk_omits_target, decide(.aarch64, .macos, .{
        .sdk_path = "/sdk",
        .libsystem_tbd = broken_clt_tbd,
    }).reason);
    try testing.expectEqual(Reason.sdk_stub_unrecognized, decide(.aarch64, .macos, .{
        .sdk_path = "/sdk",
        .libsystem_tbd = "--- !tapi-tbd\ninstall-name: '/usr/lib/libSystem.B.dylib'\n",
    }).reason);
    try testing.expectEqual(Reason.sdk_declares_target, decide(.aarch64, .macos, .{
        .sdk_path = "/sdk",
        .libsystem_tbd = healthy_tbd,
    }).reason);
    try testing.expectEqual(Reason.not_arm64_macos_host, decide(.x86_64, .linux, .{}).reason);
}

test "every reason explains itself in one non-empty line" {
    inline for (@typeInfo(Reason).@"enum".fields) |field| {
        const reason: Reason = @enumFromInt(field.value);
        const text = reason.explain();
        try testing.expect(text.len > 0);
        try testing.expect(std.mem.indexOfScalar(u8, text, '\n') == null);
    }
}

test "pinned choice is an explicit, non-native query" {
    const q = Choice.pinned_macos_arm64.query(null);
    try testing.expectEqual(std.Target.Cpu.Arch.aarch64, q.cpu_arch.?);
    try testing.expectEqual(std.Target.Os.Tag.macos, q.os_tag.?);
    try testing.expect(q.os_version_min == null);
    try testing.expect(q.os_version_max == null);
    try testing.expect(!q.isNativeOs());
    try testing.expect(Choice.native.query(null).isNativeOs());
}

test "a known host version is pinned as both ends of the range" {
    const host: std.SemanticVersion = .{ .major = 26, .minor = 1, .patch = 2 };
    const q = Choice.pinned_macos_arm64.query(host);

    try testing.expectEqual(std.Target.Os.Tag.macos, q.os_tag.?);
    try testing.expect(!q.isNativeOs());
    try testing.expectEqual(@as(u32, 26), q.os_version_min.?.semver.major);
    try testing.expectEqual(@as(u32, 1), q.os_version_min.?.semver.minor);
    try testing.expectEqual(@as(u32, 2), q.os_version_min.?.semver.patch);
    try testing.expect(q.os_version_min.?.semver.order(q.os_version_max.?.semver) == .eq);

    // The native choice never carries a version: it has no explicit target at all.
    const native = Choice.native.query(host);
    try testing.expect(native.os_version_min == null);
    try testing.expect(native.isNativeOs());
}

test "only a plausible Apple silicon version is pinned" {
    try testing.expect(pinnedOsVersion(null) == null);

    // No Apple silicon Mac runs macOS 10.x, so such a reading is not trusted.
    try testing.expect(pinnedOsVersion(.{ .major = 10, .minor = 15, .patch = 7 }) == null);

    const big_sur = pinnedOsVersion(.{ .major = 11, .minor = 0, .patch = 0 }).?;
    try testing.expectEqual(@as(u32, 11), big_sur.major);

    // Pre-release and build metadata have no meaning as a deployment target.
    const tagged = pinnedOsVersion(.{
        .major = 15,
        .minor = 3,
        .patch = 1,
        .pre = "beta.2",
        .build = "24D60",
    }).?;
    try testing.expect(tagged.pre == null);
    try testing.expect(tagged.build == null);
    try testing.expectEqual(@as(u32, 3), tagged.minor);
}

test "auto resolves to exactly what the probe found" {
    inline for (.{
        .{ macos_host.SdkProbe{ .sdk_path = "/sdk", .libsystem_tbd = broken_clt_tbd }, Choice.pinned_macos_arm64, Reason.sdk_omits_target },
        .{ macos_host.SdkProbe{ .sdk_path = "/sdk", .libsystem_tbd = healthy_tbd }, Choice.native, Reason.sdk_declares_target },
        .{ macos_host.SdkProbe{}, Choice.pinned_macos_arm64, Reason.sdk_not_probed },
    }) |case| {
        const r = resolve(.auto, .aarch64, .macos, case[0]);
        try testing.expectEqual(case[1], r.effective.choice);
        try testing.expectEqual(case[2], r.effective.reason);

        // Under auto the two halves are the same fact, so nothing is overridden.
        try testing.expectEqual(r.observed.choice, r.effective.choice);
        try testing.expectEqual(r.observed.reason, r.effective.reason);
        try testing.expect(!r.overridesProbe());
    }
}

test "a forced selection never borrows the probe's finding" {
    // The bug this guards: -Dmacos-libsystem=sdk used to be recorded as
    // `sdk_declares_target`, i.e. as though the stub had been read and had
    // listed arm64-macos, on a machine whose stub does the opposite.
    const broken: macos_host.SdkProbe = .{ .sdk_path = "/sdk", .libsystem_tbd = broken_clt_tbd };

    const forced_sdk = resolve(.sdk, .aarch64, .macos, broken);
    try testing.expectEqual(Choice.native, forced_sdk.effective.choice);
    try testing.expectEqual(Reason.forced_sdk_stub, forced_sdk.effective.reason);
    try testing.expectEqual(Reason.sdk_omits_target, forced_sdk.observed.reason);
    try testing.expect(forced_sdk.overridesProbe());

    const healthy: macos_host.SdkProbe = .{ .sdk_path = "/sdk", .libsystem_tbd = healthy_tbd };
    const forced_bundled = resolve(.bundled, .aarch64, .macos, healthy);
    try testing.expectEqual(Choice.pinned_macos_arm64, forced_bundled.effective.choice);
    try testing.expectEqual(Reason.forced_bundled_stub, forced_bundled.effective.reason);
    try testing.expectEqual(Reason.sdk_declares_target, forced_bundled.observed.reason);
    try testing.expect(forced_bundled.overridesProbe());
}

test "a force that agrees with the probe is not reported as an override" {
    const broken: macos_host.SdkProbe = .{ .sdk_path = "/sdk", .libsystem_tbd = broken_clt_tbd };
    const forced = resolve(.bundled, .aarch64, .macos, broken);

    try testing.expectEqual(Choice.pinned_macos_arm64, forced.effective.choice);
    try testing.expect(!forced.overridesProbe());

    // Agreeing on the choice is still not the same reason: the force is why.
    try testing.expectEqual(Reason.forced_bundled_stub, forced.effective.reason);
    try testing.expectEqual(Reason.sdk_omits_target, forced.observed.reason);
}

test "off macOS a force still resolves, and the observation stays honest" {
    // A Linux checkout uses -Dmacos-libsystem=bundled to exercise the Mach-O
    // link path, and the report must not claim anything about an SDK there.
    const linux = resolve(.bundled, .x86_64, .linux, .{});
    try testing.expectEqual(Choice.pinned_macos_arm64, linux.effective.choice);
    try testing.expectEqual(Reason.forced_bundled_stub, linux.effective.reason);
    try testing.expectEqual(Reason.not_arm64_macos_host, linux.observed.reason);
    try testing.expect(linux.overridesProbe());

    try testing.expectEqual(Choice.native, resolve(.auto, .x86_64, .linux, .{}).effective.choice);
}

test "every selection is handled and each maps to one effective choice" {
    inline for (@typeInfo(Selection).@"enum".fields) |field| {
        const selection: Selection = @enumFromInt(field.value);
        const r = resolve(selection, .aarch64, .macos, .{ .sdk_path = "/sdk", .libsystem_tbd = healthy_tbd });
        const expected: Choice = switch (selection) {
            .auto, .sdk => .native,
            .bundled => .pinned_macos_arm64,
        };
        try testing.expectEqual(expected, r.effective.choice);
        try testing.expect(r.effective.reason.explain().len > 0);
    }
}

test "a target matching the build host is not excused from running" {
    // This is the case the macOS gate rests on: on an arm64 Mac the pinned
    // aarch64-macos target IS the host, so a run that cannot happen must fail
    // rather than be forgiven.
    try testing.expect(targetRunsOnBuildHost(.aarch64, .macos, .aarch64, .macos));
    try testing.expect(targetRunsOnBuildHost(.x86_64, .linux, .x86_64, .linux));
    try testing.expect(targetRunsOnBuildHost(.aarch64, .linux, .aarch64, .linux));
}

test "a foreign target is excused, which is what a Linux link check needs" {
    // `zig build test -Dtarget=aarch64-macos` from Linux compiles and links a
    // Mach-O it cannot execute; excusing the run is what makes that a usable
    // check off a Mac.
    try testing.expect(!targetRunsOnBuildHost(.aarch64, .macos, .x86_64, .linux));
    try testing.expect(!targetRunsOnBuildHost(.aarch64, .macos, .aarch64, .linux));
}

test "neither architecture nor operating system alone makes a host" {
    // Same OS, other architecture.
    try testing.expect(!targetRunsOnBuildHost(.x86_64, .macos, .aarch64, .macos));
    // Same architecture, other OS.
    try testing.expect(!targetRunsOnBuildHost(.aarch64, .linux, .aarch64, .macos));
}

test "Rosetta is not assumed while deciding to forgive a missing run" {
    // An arm64 Mac really can execute x86_64-macos under translation, but
    // assuming it here would forgive a run on the strength of a facility that
    // may be absent. Assuming less makes an impossible run loud.
    try testing.expect(!targetRunsOnBuildHost(.x86_64, .macos, .aarch64, .macos));
}

test "the pinned #899 target is the host on the machine the rule is for" {
    // The selection rule and the run excuse have to agree: the choice the rule
    // makes on an affected Mac must be one that machine can execute, or the
    // gate measures nothing.
    const decision = decide(.aarch64, .macos, .{ .sdk_path = "/sdk", .libsystem_tbd = broken_clt_tbd });
    try testing.expectEqual(Choice.pinned_macos_arm64, decision.choice);

    const pinned = decision.choice.query(null);
    try testing.expect(targetRunsOnBuildHost(
        pinned.cpu_arch.?,
        pinned.os_tag.?,
        .aarch64,
        .macos,
    ));
}
