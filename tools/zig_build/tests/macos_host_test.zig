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
const tbdDeclaresTarget = macos_host.tbdDeclaresTarget;

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

    // Right arch, wrong platform: the two halves have to agree.
    try testing.expectEqual(TbdVerdict.omits, classifyTbd(v3_ios_tbd, required_target));
    try testing.expectEqual(@as(?bool, true), archsFieldDeclares(v3_ios_tbd, "arm64", "ios"));
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
