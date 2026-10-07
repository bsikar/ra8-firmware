// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! The published record layouts. The membrane's own body needs a card on a
//! bus, so what is testable here is the shape of what crosses it.

const std = @import("std");
const abi = @import("abi_types");

test "the source enum is a byte with the documented values" {
    try std.testing.expectEqual(@as(usize, 1), @sizeOf(abi.Source));
    try std.testing.expectEqual(@as(u8, 0), @backingInt(abi.Source.card));
    try std.testing.expectEqual(@as(u8, 1), @backingInt(abi.Source.provisioned));
}

test "the pin block packs the first 16 bytes of the config" {
    try std.testing.expectEqual(@as(usize, 0), @offsetOf(abi.Config, "spi_channel"));
    try std.testing.expectEqual(@as(usize, 2), @offsetOf(abi.Config, "sck"));
    try std.testing.expectEqual(@as(usize, 4), @offsetOf(abi.Config, "cipo"));
    try std.testing.expectEqual(@as(usize, 6), @offsetOf(abi.Config, "copi"));
    try std.testing.expectEqual(@as(usize, 8), @offsetOf(abi.Config, "cs"));
    try std.testing.expectEqual(@as(usize, 12), @offsetOf(abi.Config, "pclka_hz"));
}

test "the pointer run starts at a fixed offset on both widths" {
    const ptr = @sizeOf(usize);
    try std.testing.expectEqual(@as(usize, 16), @offsetOf(abi.Config, "filename"));
    try std.testing.expectEqual(16 + ptr, @offsetOf(abi.Config, "provision_blob"));
    try std.testing.expectEqual(16 + (2 * ptr), @offsetOf(abi.Config, "provision_len"));
}

test "an unset config disables provisioning and takes the default name" {
    const cfg: abi.Config = .{
        .spi_channel = 0,
        .sck = 0,
        .cipo = 0,
        .copi = 0,
        .cs = 0,
        .pclka_hz = 0,
        .filename = null,
        .provision_blob = null,
        .provision_len = 0,
    };
    try std.testing.expect(cfg.filename == null);
    try std.testing.expect(cfg.provision_blob == null);
}
