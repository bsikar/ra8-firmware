//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Decode the IT8951 I80 GET_DEV_INFO response (inc/ra8_epaper.h,
//! RA8FW-548). Pure, no bus: 20 words in, a ra8_epaper_dev_info_t out.

pub const ver_chars: usize = 16;
pub const word_count: usize = 20;

const idx_width: usize = 0;
const idx_height: usize = 1;
const idx_buf_lo: usize = 2;
const idx_buf_hi: usize = 3;
const idx_fw: usize = 4;
const idx_lut: usize = 12;

/// `ra8_epaper_dev_info_t`: width, height, frame-RAM base, then the two
/// NUL-terminated version strings.
pub const DevInfo = extern struct {
    panel_width: u16 = 0,
    panel_height: u16 = 0,
    image_buf_base: u32 = 0,
    fw_version: [ver_chars + 1]u8 = @splat(0),
    lut_version: [ver_chars + 1]u8 = @splat(0),
};

comptime {
    if (@sizeOf(DevInfo) != 44) @compileError("DevInfo must match ra8_epaper_dev_info_t");
    if (@offsetOf(DevInfo, "fw_version") != 8 or @offsetOf(DevInfo, "lut_version") != 25)
        @compileError("DevInfo field offsets drifted from the C struct");
}

/// A printable byte (0x20..0x7E) is kept; anything else becomes NUL.
pub fn printable(byte: u8) u8 {
    return if (byte >= 0x20 and byte < 0x7F) byte else 0;
}

/// Two version chars per word, high byte first.
fn unpack(word: u16, dst: []u8) void {
    dst[0] = printable(@truncate(word >> 8));
    dst[1] = printable(@truncate(word));
}

pub fn decode(words: *const [word_count]u16) DevInfo {
    var info: DevInfo = .{};
    info.panel_width = words[idx_width];
    info.panel_height = words[idx_height];
    info.image_buf_base = @as(u32, words[idx_buf_lo]) | (@as(u32, words[idx_buf_hi]) << 16);
    for (words[idx_fw..idx_lut], 0..) |word, i| unpack(word, info.fw_version[2 * i ..]);
    for (words[idx_lut..word_count], 0..) |word, i| unpack(word, info.lut_version[2 * i ..]);
    info.fw_version[ver_chars] = 0;
    info.lut_version[ver_chars] = 0;
    return info;
}
