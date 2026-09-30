//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IPv6 literal parsing and address classification. At most one `::` run is
//! accepted, a zone identifier is refused outright, and a trailing dotted quad
//! folds into the final two groups so the IPv4-mapped form classifies as IPv4.

const ascii = @import("ascii.zig");
const root = @import("root.zig");
const v4 = @import("v4.zig");

const AddrClass = root.AddrClass;
const limits = root.limits;

/// IPv6 prefix bytes that mark non-public address space.
const prefix = struct {
    pub const byte_ff: u8 = 0xFF; // multicast lead byte / mapped filler
    pub const ula_mask: u8 = 0xFE; // isolates fc00::/7
    pub const ula_value: u8 = 0xFC;
    pub const ll_lead: u8 = 0xFE; // fe80::/10
    pub const ll_mask: u8 = 0xC0;
    pub const ll_value: u8 = 0x80;
    pub const loopback_last: u8 = 0x01; // final byte of ::1
    pub const mapped_ff_a: usize = 10; // first 0xFF of the mapped prefix
    pub const mapped_ff_b: usize = 11;
    pub const last: usize = 15;
};

/// Running state of one literal parse.
///
/// The `::` run splits the groups into a head and a tail; the tail packs flush
/// against the end of the address, so the zero fill is whatever sits between.
const Parse = struct {
    head: [limits.v6_groups]u16 = .{0} ** limits.v6_groups,
    tail: [limits.v6_groups]u16 = .{0} ** limits.v6_groups,
    head_count: usize = 0,
    tail_count: usize = 0,
    seen_run: bool = false,

    /// Append one group to whichever side of the `::` is active.
    fn push(self: *Parse, group: u16) bool {
        const dst = if (self.seen_run) &self.tail else &self.head;
        const count = if (self.seen_run) &self.tail_count else &self.head_count;
        if (count.* == limits.v6_groups) return false;
        dst[count.*] = group;
        count.* += 1;
        return true;
    }
};

/// What follows a group: the literal ended, another group follows, or refuse.
const Step = enum { done, more, bad };

/// Parse an IPv6 literal spanning the whole of `text`.
pub fn parse(text: []const u8) ?[limits.v6_bytes]u8 {
    var st = Parse{};
    var at: usize = 0;

    if (ascii.byteAt(text, 0) == ':') {
        if (ascii.byteAt(text, 1) != ':') return null;
        st.seen_run = true;
        at = 2;
    }

    while (ascii.byteAt(text, at) != 0) {
        if (ascii.byteAt(text, at) == ':') {
            if (st.seen_run or ascii.byteAt(text, at + 1) != ':') return null;
            st.seen_run = true;
            at += 2;
            continue;
        }
        const rest = text[at..];
        if (quadIsNext(rest)) {
            if (!takeQuad(rest, &st)) return null;
            return finish(&st);
        }
        const group = readGroup(text, &at) orelse return null;
        if (!st.push(group)) return null;
        switch (stepSeparator(text, &at)) {
            .bad => return null,
            .done => break,
            .more => {},
        }
    }
    return finish(&st);
}

/// Classify sixteen parsed address bytes.
///
/// A mapped address is unwrapped and classified as IPv4, so a mapped loopback
/// cannot slip past the loopback refusal.
pub fn classify(b: [limits.v6_bytes]u8) AddrClass {
    if (isV4Mapped(b)) {
        return v4.classify(b[limits.v6_mapped_v4..][0..limits.v4_bytes].*);
    }
    if (isLoopback(b)) return .loopback;
    if (isUnspecified(b)) return .unknown;
    if (b[0] == prefix.byte_ff) return .unknown; // multicast
    if (b[0] == prefix.ll_lead and (b[1] & prefix.ll_mask) == prefix.ll_value) return .linklocal;
    if ((b[0] & prefix.ula_mask) == prefix.ula_value) return .private;
    return .public;
}

/// Whether the group at the cursor is the trailing dotted quad, not a later one.
fn quadIsNext(rest: []const u8) bool {
    const dot = indexOf(rest, '.') orelse return false;
    const colon = indexOf(rest, ':') orelse return true;
    return dot < colon;
}

fn indexOf(s: []const u8, needle: u8) ?usize {
    for (s, 0..) |c, i| {
        if (c == needle) return i;
    }
    return null;
}

/// Consume a trailing dotted quad as the final two groups.
fn takeQuad(rest: []const u8, st: *Parse) bool {
    const quad = v4.parse(rest) orelse return false;
    const hi = (@as(u16, quad[0]) << 8) | quad[1];
    const lo = (@as(u16, quad[2]) << 8) | quad[3];
    return st.push(hi) and st.push(lo);
}

/// Read one group of up to four hex digits, advancing the cursor past it.
fn readGroup(text: []const u8, at: *usize) ?u16 {
    var value: u32 = 0;
    var digits: usize = 0;
    while (ascii.hexValue(ascii.byteAt(text, at.*))) |digit| {
        if (digits == limits.v6_group_hex) return null;
        value = (value * 16) + digit;
        digits += 1;
        at.* += 1;
    }
    if (digits == 0) return null;
    return @intCast(value & 0xFFFF);
}

/// Step the cursor past the separator that follows one group.
///
/// A single trailing colon is not a run, and any other trailing byte (a zone
/// identifier, for one) ends the parse in refusal.
fn stepSeparator(text: []const u8, at: *usize) Step {
    if (ascii.byteAt(text, at.*) == 0) return .done;
    if (ascii.byteAt(text, at.*) != ':') return .bad;
    const next = ascii.byteAt(text, at.* + 1);
    if (next == 0) return .bad;
    if (next != ':') at.* += 1;
    return .more;
}

/// Check the group budget and pack the parse state into sixteen bytes.
///
/// Without a `::` the literal must carry every group; with one it must carry
/// strictly fewer, since the run stands for at least one group.
fn finish(st: *const Parse) ?[limits.v6_bytes]u8 {
    const total = st.head_count + st.tail_count;
    if (st.seen_run) {
        if (total >= limits.v6_groups) return null;
    } else if (total != limits.v6_groups) {
        return null;
    }
    var out: [limits.v6_bytes]u8 = .{0} ** limits.v6_bytes;
    for (0..st.head_count) |i| {
        out[i * 2] = @intCast(st.head[i] >> 8);
        out[(i * 2) + 1] = @intCast(st.head[i] & 0xFF);
    }
    const base = limits.v6_groups - st.tail_count;
    for (0..st.tail_count) |i| {
        const g = base + i;
        out[g * 2] = @intCast(st.tail[i] >> 8);
        out[(g * 2) + 1] = @intCast(st.tail[i] & 0xFF);
    }
    return out;
}

fn isV4Mapped(b: [limits.v6_bytes]u8) bool {
    for (0..prefix.mapped_ff_a) |i| {
        if (b[i] != 0) return false;
    }
    return (b[prefix.mapped_ff_a] == prefix.byte_ff) and (b[prefix.mapped_ff_b] == prefix.byte_ff);
}

fn isLoopback(b: [limits.v6_bytes]u8) bool {
    for (0..prefix.last) |i| {
        if (b[i] != 0) return false;
    }
    return b[prefix.last] == prefix.loopback_last;
}

fn isUnspecified(b: [limits.v6_bytes]u8) bool {
    for (b) |byte| {
        if (byte != 0) return false;
    }
    return true;
}
