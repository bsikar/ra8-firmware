//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The image header an MRAM slot carries, and the two predicates that decide
//! whether an image may be trusted: is the stored header consistent, and may
//! this image be copied to the run base and launched.

const std = @import("std");

/// MRAM/SRAM layout the image rules read. Mirrors `ra8_dfu_layout_t` and
/// `ra8_dfu_run_t` in `ra8_dfu.h`.
pub const layout = struct {
    /// MRAM program page (32 bytes); also the header size.
    pub const page_size: u32 = 0x0000_0020;
    /// Largest image body a slot holds (slot size less the header page).
    pub const img_max: u32 = 0x0006_FFE0;
    /// Valid-image header magic, "RA8D".
    pub const hdr_magic: u32 = 0x5241_3844;
    /// The fixed SRAM copy-to-run base every payload is linked at.
    pub const run_base: u32 = 0x2202_0000;
};

/// The 32-byte image header, programmed last into a slot so a torn write
/// leaves the slot invalid. Layout is the C `ra8_dfu_img_hdr_t`.
pub const Header = extern struct {
    magic: u32,
    seq: u32,
    img_len: u32,
    img_crc32: u32,
    entry: u32,
    rsv0: u32,
    rsv1: u32,
    rsv2: u32,
};

comptime {
    std.debug.assert(@sizeOf(Header) == layout.page_size);
}

/// A body length is usable when it is non-empty, fits the slot, and lands on
/// a whole program page.
pub fn lengthValid(img_len: u32) bool {
    return img_len != 0 and img_len <= layout.img_max and img_len % layout.page_size == 0;
}

/// Whether `hdr` describes an image whose body hashes to `computed_crc`.
pub fn headerValid(hdr: *const Header, computed_crc: u32) bool {
    return hdr.magic == layout.hdr_magic and
        lengthValid(hdr.img_len) and
        computed_crc == hdr.img_crc32;
}

/// Whether an image recording `entry` and `img_len` may be copied to the run
/// base and launched. `entry` is checked against the trusted constant; the
/// jump itself always goes to that constant, never to `entry`.
pub fn runTargetValid(entry: u32, img_len: u32) bool {
    return entry == layout.run_base and lengthValid(img_len);
}

test "header size is one program page" {
    try std.testing.expectEqual(@as(usize, 32), @sizeOf(Header));
}
