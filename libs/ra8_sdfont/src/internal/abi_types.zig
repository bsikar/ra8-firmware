//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The record layouts `inc/ra8_sdfont.h` publishes: what callers build on
//! their own stacks and what they read back.
//!
//! Split from the membrane so the layout is checked on the host without
//! linking the bus: the membrane names pin, SPI, SD and filesystem symbols
//! that only exist in a firmware link.

const std = @import("std");

/// `ra8_sdfont_source_t`: where the bytes that came back were read from.
pub const Source = enum(u8) {
    card = 0,
    provisioned = 1,
    _,
};

/// `ra8_sdfont_cfg_t`: the bus description plus the optional fallback blob.
/// Built on caller stacks by the example apps, so the layout is the contract.
pub const Config = extern struct {
    spi_channel: u8,
    sck: u16,
    cipo: u16,
    copi: u16,
    cs: u16,
    pclka_hz: u32,
    filename: ?[*:0]const u8,
    provision_blob: ?[*]const u8,
    provision_len: u32,
};

comptime {
    const ptr = @sizeOf(usize);

    std.debug.assert(@offsetOf(Config, "spi_channel") == 0);
    std.debug.assert(@offsetOf(Config, "sck") == 2);
    std.debug.assert(@offsetOf(Config, "cipo") == 4);
    std.debug.assert(@offsetOf(Config, "copi") == 6);
    std.debug.assert(@offsetOf(Config, "cs") == 8);
    std.debug.assert(@offsetOf(Config, "pclka_hz") == 12);

    // The four pins pack the first 16 bytes on both widths, so the pointer
    // run starts at a fixed offset and only its stride changes.
    std.debug.assert(@offsetOf(Config, "filename") == 16);
    std.debug.assert(@offsetOf(Config, "provision_blob") == 16 + ptr);
    std.debug.assert(@offsetOf(Config, "provision_len") == 16 + (2 * ptr));

    std.debug.assert(@sizeOf(Source) == 1);
    std.debug.assert(@backingInt(Source.card) == 0);
    std.debug.assert(@backingInt(Source.provisioned) == 1);
}
