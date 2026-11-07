//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const fs = @import("ra8_fs");

test "sector arena serves four distinct, stable role buffers" {
    const ab = fs.fat_bytes;
    const w = ab.priv_sec_walk();
    const f = ab.priv_sec_fat();
    const f2 = ab.priv_sec_fat2();
    const io = ab.priv_sec_io();
    try std.testing.expect(w != f);
    try std.testing.expect(f != f2);
    try std.testing.expect(f2 != io);
    try std.testing.expect(w != io);
    try std.testing.expectEqual(w, ab.priv_sec_walk());
    try std.testing.expectEqual(io, ab.priv_sec_io());
}
