//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure network-PAL core: the frame ring the stack pushes into and drains,
//! the ra8_eth status -> PAL event translation, and the two argument
//! predicates the send / receive contract is written in. No C ABI, no
//! logging, and no hardware access, so every branch is host-testable.
//!
//! The ring is a fixed four-slot FIFO over caller-sized frames: no loop
//! bound is dynamic, nothing is allocated and nothing recurses (NASA P10
//! rules 1-3), so it behaves the same on the host harness and on the RA8D2.

const std = @import("std");

/// 48-bit Ethernet MAC length (`k_ra8_net_pal_mac_addr_len`).
pub const mac_addr_len: u16 = 6;
/// Standard Ethernet payload size (`k_ra8_net_pal_mtu`).
pub const mtu: u16 = 1500;
/// MTU + header + FCS placeholder (`k_ra8_net_pal_frame_max`).
pub const frame_max: u16 = 1518;
/// In-flight frame slots (`k_ra8_net_pal_ring_slots`, TU-local in the C).
pub const ring_slots: u16 = 4;

/// Link state surfaced to the stack (`ra8_net_pal_link_state_t`).
pub const LinkState = enum(u8) {
    down = 0,
    up = 1,
};

/// 48-bit MAC container (`ra8_net_pal_mac_t`), a caller-owned layout.
pub const Mac = extern struct {
    bytes: [mac_addr_len]u8,

    /// The all-zero default `ra8_net_pal_init` programmes for a NULL mac.
    pub const zero: Mac = .{ .bytes = [_]u8{0} ** mac_addr_len };
};

/// Event bits handed to the stack callback (`ra8_net_pal_event_t`).
pub const event_none: u32 = 0x00;
/// Link came up.
pub const event_link_up: u32 = 0x01;
/// Link went down.
pub const event_link_down: u32 = 0x02;
/// RX descriptor has data.
pub const event_rx_ready: u32 = 0x04;
/// TX descriptor freed.
pub const event_tx_done: u32 = 0x08;
/// MAC reported a fault.
pub const event_error: u32 = 0x10;

/// Translate a raw `ra8_eth` status mask into PAL event bits.
///
/// The C's mapping is deliberately coarse: any status bit at all is an
/// error event, a clear mask is no event. Later waves fan the bits out.
pub fn translateEvent(eth_mask: u32) u32 {
    if (eth_mask != 0) {
        return event_error;
    }
    return event_none;
}

/// Whether a transmit length is inside the contract: non-zero, <= frame_max.
pub fn sendLenValid(len: u16) bool {
    return (len != 0) and (len <= frame_max);
}

/// Whether a receive buffer is large enough to take any frame the ring holds.
///
/// The C demands full `frame_max` capacity up front rather than comparing
/// against the queued frame, so a short buffer is rejected even when the
/// waiting frame would have fitted.
pub fn recvCapacityValid(capacity: u16) bool {
    return capacity >= frame_max;
}

/// One ring slot: a frame body plus the byte count that is live in it.
pub const Slot = struct {
    /// Zero means the slot is empty.
    len: u16 = 0,
    data: [frame_max]u8 = [_]u8{0} ** frame_max,
};

/// Fixed-depth frame FIFO shared by the send and receive primitives.
///
/// `head` is the next slot to pop, `tail` the next to push, and `count` the
/// live frame total; the cursors wrap modulo `ring_slots`.
pub const Ring = struct {
    slots: [ring_slots]Slot = [_]Slot{.{}} ** ring_slots,
    head: u16 = 0,
    tail: u16 = 0,
    count: u16 = 0,

    /// Drop every queued frame and rewind both cursors to slot zero.
    pub fn reset(self: *Ring) void {
        self.head = 0;
        self.tail = 0;
        self.count = 0;
        for (&self.slots) |*slot| {
            slot.len = 0;
        }
    }

    /// True once every slot carries a frame.
    pub fn isFull(self: *const Ring) bool {
        return self.count >= ring_slots;
    }

    /// True while no frame is queued.
    pub fn isEmpty(self: *const Ring) bool {
        return self.count == 0;
    }

    /// Copy `frame` into the next free slot; false means the ring was full.
    ///
    /// The length is the caller's contract (`sendLenValid`), asserted here
    /// rather than re-checked, which is what the C did by validating in
    /// `ra8_net_pal_send_frame` before touching the ring.
    pub fn push(self: *Ring, frame: []const u8) bool {
        std.debug.assert(frame.len > 0);
        std.debug.assert(frame.len <= frame_max);
        if (self.isFull()) {
            return false;
        }
        const slot = &self.slots[self.tail];
        @memcpy(slot.data[0..frame.len], frame);
        slot.len = @intCast(frame.len);
        self.tail = (self.tail + 1) % ring_slots;
        self.count += 1;
        return true;
    }

    /// Copy the oldest frame into `out` and free its slot.
    ///
    /// Returns the byte count written, or null when nothing is queued.
    pub fn pop(self: *Ring, out: []u8) ?u16 {
        if (self.isEmpty()) {
            return null;
        }
        const slot = &self.slots[self.head];
        const n = slot.len;
        std.debug.assert(out.len >= n);
        @memcpy(out[0..n], slot.data[0..n]);
        slot.len = 0;
        self.head = (self.head + 1) % ring_slots;
        self.count -= 1;
        return n;
    }
};

comptime {
    std.debug.assert(mtu == 1500);
    std.debug.assert(frame_max == 1518);
    std.debug.assert(mac_addr_len == 6);
    std.debug.assert(ring_slots == 4);
    std.debug.assert(@sizeOf(Mac) == 6);
    std.debug.assert(@sizeOf(LinkState) == 1);
}
