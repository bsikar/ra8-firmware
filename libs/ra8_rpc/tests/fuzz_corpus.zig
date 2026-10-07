//! Fuzz corpus seeds in the shape std.testing.Smith.slice reads back.

const std = @import("std");

/// `bytes` behind its little-endian u32 length: one Smith slice.
pub fn framed(comptime n: usize, bytes: [n]u8) [4 + n]u8 {
    var out: [4 + n]u8 = undefined;
    std.mem.writeInt(u32, out[0..4], n, .little);
    @memcpy(out[4..], &bytes);
    return out;
}

/// Every seed framed; call at comptime.
pub fn all(comptime seeds: []const []const u8) [seeds.len][]const u8 {
    var out: [seeds.len][]const u8 = undefined;
    for (seeds, &out) |seed, *slot| {
        const one = framed(seed.len, seed[0..seed.len].*);
        slot.* = &one;
    }
    return out;
}
