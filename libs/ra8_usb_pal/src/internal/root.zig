//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the USB PAL: the per-endpoint packet ring, the two
//! MC/DC-promoted predicates, and the INTSTS0 -> PAL event
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

/// INTSTS0 layout the translation reads (HUM Ch 36.2.14). Positions are bit
/// indices; the field values are already shifted for a masked compare.
pub const intsts0 = struct {
    pub const bit_brdy: u4 = 8;
    pub const bit_nrdy: u4 = 9;
    pub const bit_bemp: u4 = 10;
    pub const bit_ctrt: u4 = 11;
    pub const bit_dvst: u4 = 12;
    pub const bit_sofr: u4 = 13;
    pub const bit_rsme: u4 = 14;
    pub const bit_vbse: u4 = 15;

    pub const mask_ctsq: u16 = 0x0007;
    pub const mask_valid: u16 = 0x0008;
    pub const mask_dvsq: u16 = 0x0070;
    pub const mask_vbsts: u16 = 0x0080;

    pub const dvsq_default: u16 = 0x0010;
    pub const dvsq_suspend: u16 = 0x0040;
    pub const ctsq_sqer: u16 = 0x0006;

    fn raised(snapshot: u16, position: u4) bool {
        return (snapshot & (@as(u16, 1) << position)) != 0;
    }
};

/// DVSQ half of a device-state transition. The suspend flag rides alongside
/// the three-bit state, so it is tested first; Default is the post-bus-reset
/// state. Powered, Address and Configured have no bit in the taxonomy, so
/// they translate to nothing and the caller stays silent rather than
/// reporting a state change it cannot name.
fn dvsqEvent(snapshot: u16) u16 {
    if ((snapshot & intsts0.dvsq_suspend) != 0) return event_suspend;
    if ((snapshot & intsts0.mask_dvsq) == intsts0.dvsq_default) return event_reset;
    return event_none;
}

/// CTRT half of a control-transfer stage transition. A transition means a
/// SETUP packet only while VALID is still latched: the SETUP readers clear
/// VALID once they drain the request registers, so a transition with VALID
/// already gone is a data or status step. CTSQ == SQER is the hardware's own
/// sequence-error report, the one control-path condition called an error.
fn ctrtEvent(snapshot: u16) u16 {
    var evt: u16 = event_none;
    if ((snapshot & intsts0.mask_valid) != 0) evt |= event_setup;
    if ((snapshot & intsts0.mask_ctsq) == intsts0.ctsq_sqer) evt |= event_error;
    return evt;
}

/// Raw INTSTS0 snapshot -> PAL event mask, one arm per source bit. Bits are
/// ORed, so one snapshot can raise several at once. BRDY is the coarse arm:
/// it asserts for either direction and reports as `event_ep_out`, because
/// resolving it per pipe needs BRDYSTS, which the PAL is never given.
pub fn translateEvent(snapshot: u16) u16 {
    var evt: u16 = event_none;
    if (intsts0.raised(snapshot, intsts0.bit_sofr)) evt |= event_sof;
    if (intsts0.raised(snapshot, intsts0.bit_rsme)) evt |= event_resume;
    if (intsts0.raised(snapshot, intsts0.bit_vbse)) {
        evt |= if ((snapshot & intsts0.mask_vbsts) != 0) event_attach else event_detach;
    }
    if (intsts0.raised(snapshot, intsts0.bit_dvst)) evt |= dvsqEvent(snapshot);
    if (intsts0.raised(snapshot, intsts0.bit_ctrt)) evt |= ctrtEvent(snapshot);
    if (intsts0.raised(snapshot, intsts0.bit_brdy)) evt |= event_ep_out;
    if (intsts0.raised(snapshot, intsts0.bit_bemp)) evt |= event_ep_in;
    if (intsts0.raised(snapshot, intsts0.bit_nrdy)) evt |= event_error;
    return evt;
}

/// Strip the USB descriptor direction bit so callers may pass either the
/// descriptor form (0x83) or the bare endpoint number (3).
pub fn maskEpAddr(ep_addr: u8) u8 {
    return ep_addr & ep_addr_mask;
}

/// `ra8_usb_pal_init` accepts exactly the two controller selectors.
pub fn speedValid(raw: u8) bool {
    return raw == @backingInt(Speed.fs) or raw == @backingInt(Speed.hs);
}

/// Direction byte carried into `ra8_usb_pal_ep_open` by value.
pub fn dirValid(raw: u8) bool {
    return raw == @backingInt(EpDir.out) or raw == @backingInt(EpDir.in);
}

/// Transfer type plus packet-size rule, exactly the C's single `if`.
pub fn typeAndPacketValid(raw_type: u8, max_packet: u16) bool {
    return !(raw_type > @backingInt(EpType.intr) or max_packet == 0 or max_packet > xfer_max);
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
    ring: [ring_slots]Packet = @splat(.{}),

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
    eps: [ep_table_len]EpSlot = @splat(.{}),

    pub fn resetAll(self: *Table) void {
        for (&self.eps) |*slot| slot.reset();
    }

    pub fn at(self: *Table, ep_addr: u8) *EpSlot {
        return &self.eps[ep_addr];
    }
};
