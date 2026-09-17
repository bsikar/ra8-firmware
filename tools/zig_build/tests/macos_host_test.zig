//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Unit tests for the #899 host-target selection rule.

const std = @import("std");
const macos_host = @import("macos_host");

const testing = std.testing;
const Choice = macos_host.Choice;
const decide = macos_host.decide;
const required_target = macos_host.required_target;
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

test "only an arm64 Mac with a broken SDK gets pinned" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .libsystem_tbd = broken_clt_tbd }));
    try testing.expectEqual(Choice.native, decide(.aarch64, .macos, .{ .libsystem_tbd = healthy_tbd }));
    try testing.expectEqual(Choice.native, decide(.x86_64, .macos, .{ .libsystem_tbd = broken_clt_tbd }));
    try testing.expectEqual(Choice.native, decide(.aarch64, .linux, .{}));
    try testing.expectEqual(Choice.native, decide(.x86_64, .linux, .{}));
}

test "an unreadable SDK falls back to the bundled stub on arm64 macOS" {
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{}));
    try testing.expectEqual(Choice.pinned_macos_arm64, decide(.aarch64, .macos, .{ .sdk_path = "/nope" }));
}

test "pinned choice is an explicit, non-native query" {
    const q = Choice.pinned_macos_arm64.query();
    try testing.expectEqual(std.Target.Cpu.Arch.aarch64, q.cpu_arch.?);
    try testing.expectEqual(std.Target.Os.Tag.macos, q.os_tag.?);
    try testing.expect(!q.isNativeOs());
    try testing.expect(Choice.native.query().isNativeOs());
}
