//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Fixed-capacity little-endian base-2^32 unsigned integer, the only storage
//! the decimal conversion uses.
//!
//! Every operation that can outgrow the capacity says so in its return type
//! rather than truncating, because the conversion treats "does not fit" as a
//! refusal to convert, not as an approximation.

const std = @import("std");

/// Storage bounds. 48 words hold 17 significant digits multiplied or divided
/// by 10^400 with room for the long division's shifted trial divisors.
pub const limits = struct {
    pub const words = 48;
    pub const word_bits = 32;
};

pub const Overflow = error{Capacity};

const Big = @This();

word: [limits.words]u32 = @splat(0),
used: u8 = 1,

/// The magnitude of one u64, normalized.
pub fn fromU64(value: u64) Big {
    var big = Big{};
    big.word[0] = @truncate(value);
    big.word[1] = @truncate(value >> limits.word_bits);
    big.used = if (big.word[1] != 0) 2 else 1;
    return big;
}

pub fn isZero(self: Big) bool {
    return self.used == 1 and self.word[0] == 0;
}

/// The significant words, high word last. Zero keeps one word.
fn magnitude(self: *const Big) []const u32 {
    return self.word[0..self.used];
}

/// Drop high zero words, keeping one word for zero.
fn trim(self: *Big) void {
    while (self.used > 1 and self.word[self.used - 1] == 0) self.used -= 1;
}

/// Significant bit count. Zero has none.
pub fn bitLength(self: Big) u16 {
    if (self.isZero()) return 0;
    const high = self.word[self.used - 1];
    const high_bits = limits.word_bits - @clz(high);
    return @as(u16, self.used - 1) * limits.word_bits + high_bits;
}

/// Multiply in place by a small factor.
pub fn mulSmall(self: *Big, factor: u32) Overflow!void {
    var carry: u64 = 0;
    for (self.word[0..self.used]) |*w| {
        const product = @as(u64, w.*) * factor + carry;
        w.* = @truncate(product);
        carry = product >> limits.word_bits;
    }
    if (carry == 0) return;
    if (self.used >= limits.words) return Overflow.Capacity;
    self.word[self.used] = @truncate(carry);
    self.used += 1;
}

/// This value shifted left by an exact bit count, as a new integer.
pub fn shiftLeft(self: *const Big, shift: u16) Overflow!Big {
    const whole = shift / limits.word_bits;
    const bits: u5 = @intCast(shift % limits.word_bits);
    const spill: u16 = if (bits != 0) 1 else 0;
    if (@as(u16, self.used) + whole + spill > limits.words) return Overflow.Capacity;

    var out = Big{};
    var carry: u32 = 0;
    for (self.magnitude(), 0..) |w, i| {
        out.word[i + whole] = (w << bits) | carry;
        // Shifting a u32 by 32 is undefined width, so the complement is taken
        // in u64 and narrowed; bits == 0 must contribute no carry at all.
        carry = if (bits == 0) 0 else @truncate(@as(u64, w) >> @intCast(limits.word_bits - @as(u16, bits)));
    }
    out.used = @intCast(@as(u16, self.used) + whole);
    if (carry != 0) {
        out.word[out.used] = carry;
        out.used += 1;
    }
    out.trim();
    return out;
}

/// Unsigned three-way ordering.
pub fn order(self: *const Big, other: *const Big) std.math.Order {
    if (self.used != other.used) return if (self.used > other.used) .gt else .lt;
    var i = self.used;
    while (i > 0) {
        i -= 1;
        if (self.word[i] != other.word[i]) {
            return if (self.word[i] > other.word[i]) .gt else .lt;
        }
    }
    return .eq;
}

/// Subtract a no-larger value in place.
pub fn subtract(self: *Big, other: *const Big) void {
    std.debug.assert(self.order(other) != .lt);
    var borrow: u64 = 0;
    for (self.word[0..self.used], 0..) |*w, i| {
        const taken = @as(u64, if (i < other.used) other.word[i] else 0) + borrow;
        const current: u64 = w.*;
        w.* = @truncate(current -% taken);
        borrow = if (current < taken) 1 else 0;
    }
    self.trim();
}
