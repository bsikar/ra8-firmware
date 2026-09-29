//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the USB PAL: the per-endpoint packet ring, the two
//! MC/DC-promoted predicates, and the ra8_usb status -> PAL event
//! translation. Nothing here touches the C ABI, the ra8_usb driver, or
//! the logger, so every rule is testable on its own.

const std = @import("std");

// =============================================================================
// Limits (mirror ra8_usb_pal_limits_t and the private ring dimensions)
// =============================================================================

pub const ep_max: u8 = 10;
pub const ep0_max_packet: u16 = 64;
pub const bulk_max_fs: u16 = 64;
pub const bulk_max_hs: u16 = 512;
pub const xfer_max: u16 = 1024;
pub const ep_addr_mask: u8 = 0x7F;

pub const ep_table_len: u16 = 11;
pub const ring_slots: u16 = 4;
pub const pkt_max: u16 = 1024;

// =============================================================================
// Enumerations carried across the C ABI
// =============================================================================

pub const EpDir = enum(u8) { out = 0, in = 1 };
pub const EpType = enum(u8) { control = 0, iso = 1, bulk = 2, intr = 3 };

pub const PalState = enum(u8) {
    detached = 0,
    attached = 1,
    default = 2,
    addressed = 3,
    configd = 4,
    suspended = 5,
};

pub const Speed = enum(u8) { fs = 0, hs = 1 };

pub const event_none: u16 = 0x0000;
pub const event_reset: u16 = 0x0001;
pub const event_suspend: u16 = 0x0002;
pub const event_resume: u16 = 0x0004;
pub const event_setup: u16 = 0x0008;
pub const event_ep_in: u16 = 0x0010;
pub const event_ep_out: u16 = 0x0020;
pub const event_sof: u16 = 0x0040;
pub const event_attach: u16 = 0x0080;
pub const event_detach: u16 = 0x0100;
pub const event_error: u16 = 0x8000;

comptime {
    std.debug.assert(@sizeOf(EpDir) == 1);
    std.debug.assert(@sizeOf(EpType) == 1);
    std.debug.assert(@sizeOf(PalState) == 1);
    std.debug.assert(@sizeOf(Speed) == 1);
    std.debug.assert(pkt_max == xfer_max);
    std.debug.assert(ep_table_len == @as(u16, ep_max) + 1);
}

// =============================================================================
// Promoted predicates (the C exposes these so MC/DC vectors can drive them)
// =============================================================================

/// Dispatch gate for the ra8_usb event handler: a callback must be installed
/// AND the translated mask must be something other than "no event".
pub fn shouldDispatchEvent(event_fn: ?*const anyopaque, mask: u16, none_value: u16) bool {
    return (event_fn != null) and (mask != none_value);
}

/// Endpoint-number reject predicate. EP0 is reserved (the PAL's table is
/// 1-based for callers) and anything above `limit` has no slot.
pub fn epOutOfRange(ep_addr: u8, limit: u8) bool {
    return (ep_addr == 0) or (ep_addr > limit);
}

/// ra8_usb status mask -> PAL event mask. Today any raised status bit is
/// reported as a controller error; later waves fan the bits out.
pub fn translate(usb_mask: u16) u16 {
    if (usb_mask != 0) return event_error;
    return event_none;
}

/// Strip the USB descriptor direction bit so callers may pass either the
/// descriptor form (0x83) or the bare endpoint number (3).
pub fn maskEpAddr(ep_addr: u8) u8 {
    return ep_addr & ep_addr_mask;
}

/// `ra8_usb_pal_init` accepts exactly the two controller selectors.
pub fn speedValid(raw: u8) bool {
    return raw == @intFromEnum(Speed.fs) or raw == @intFromEnum(Speed.hs);
}

/// Direction byte carried into `ra8_usb_pal_ep_open` by value.
pub fn dirValid(raw: u8) bool {
    return raw == @intFromEnum(EpDir.out) or raw == @intFromEnum(EpDir.in);
}

/// Transfer type plus packet-size rule, exactly the C's single `if`.
pub fn typeAndPacketValid(raw_type: u8, max_packet: u16) bool {
    return !(raw_type > @intFromEnum(EpType.intr) or max_packet == 0 or max_packet > xfer_max);
}

// =============================================================================
// Per-endpoint ring
// =============================================================================

pub const Packet = struct {
    len: u16 = 0,
    data: [pkt_max]u8 = undefined,
};

pub const PushFault = error{Full};
pub const PopFault = error{Empty};

pub const EpSlot = struct {
    opened: bool = false,
    dir: EpDir = .out,
    type: EpType = .control,
    max_packet: u16 = 0,
    head: u16 = 0,
    tail: u16 = 0,
    count: u16 = 0,
    ring: [ring_slots]Packet = [_]Packet{.{}} ** ring_slots,

    /// Cursor + length reset. Leaves payload bytes alone, as the C did.
    pub fn resetRing(self: *EpSlot) void {
        self.head = 0;
        self.tail = 0;
        self.count = 0;
        for (&self.ring) |*pkt| pkt.len = 0;
    }

    /// Full reset to the unopened OUT/control defaults.
    pub fn reset(self: *EpSlot) void {
        self.opened = false;
        self.dir = .out;
        self.type = .control;
        self.max_packet = 0;
        self.resetRing();
    }

    pub fn open(self: *EpSlot, dir: EpDir, ep_type: EpType, max_packet: u16) void {
        self.opened = true;
        self.dir = dir;
        self.type = ep_type;
        self.max_packet = max_packet;
        self.resetRing();
    }

    pub fn isFull(self: *const EpSlot) bool {
        return self.count >= ring_slots;
    }

    pub fn isEmpty(self: *const EpSlot) bool {
        return self.count == 0;
    }

    /// Queue one packet. `data` may be empty (zero-length packet is legal).
    pub fn push(self: *EpSlot, data: []const u8) PushFault!void {
        if (self.isFull()) return PushFault.Full;
        const pkt = &self.ring[self.tail];
        for (data, 0..) |byte, i| pkt.data[i] = byte;
        pkt.len = @intCast(data.len);
        self.tail = (self.tail + 1) % ring_slots;
        self.count += 1;
    }

    /// Pop the head packet into `out`, truncating to `out.len`. Returns the
    /// number of bytes written.
    pub fn pop(self: *EpSlot, out: []u8) PopFault!u16 {
        if (self.isEmpty()) return PopFault.Empty;
        const pkt = &self.ring[self.head];
        const n: u16 = if (pkt.len < out.len) pkt.len else @intCast(out.len);
        var i: u16 = 0;
        while (i < n) : (i += 1) out[i] = pkt.data[i];
        pkt.len = 0;
        self.head = (self.head + 1) % ring_slots;
        self.count -= 1;
        return n;
    }
};

pub const Table = struct {
    eps: [ep_table_len]EpSlot = [_]EpSlot{.{}} ** ep_table_len,

    pub fn resetAll(self: *Table) void {
        for (&self.eps) |*slot| slot.reset();
    }

    pub fn at(self: *Table, ep_addr: u8) *EpSlot {
        return &self.eps[ep_addr];
    }
};
