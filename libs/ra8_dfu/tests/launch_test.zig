//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The copy-to-run guards: which hand-offs are allowed to copy at all.

const std = @import("std");
const image = @import("image");
const launch = @import("launch");

const a_source: usize = 0x2000_0000;
const a_page: u32 = 0x20;

test "a null source never copies" {
    try std.testing.expect(!launch.mayCopy(0, a_page, image.layout.run_base));
}

test "an entry that is not the run base never copies" {
    try std.testing.expect(!launch.mayCopy(a_source, a_page, 0xDEAD_BEEF));
}

test "a bad length never copies, whatever the entry says" {
    try std.testing.expect(!launch.mayCopy(a_source, 0, image.layout.run_base));
    try std.testing.expect(!launch.mayCopy(a_source, a_page + 1, image.layout.run_base));
    try std.testing.expect(!launch.mayCopy(
        a_source,
        image.layout.img_max + a_page,
        image.layout.run_base,
    ));
}

test "a page-aligned body at the run base copies" {
    try std.testing.expect(launch.mayCopy(a_source, a_page, image.layout.run_base));
    try std.testing.expect(launch.mayCopy(a_source, image.layout.img_max, image.layout.run_base));
}
