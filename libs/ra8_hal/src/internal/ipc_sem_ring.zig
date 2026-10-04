//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! IPC hardware semaphores, cross-core NMI and the semaphore-guarded
//! SPSC ring (HUM Ch 3), RA8FW-606. Pure logic; the C ABI lives in
//! ipc_sem_ring_abi.zig. `hw` provides read32(addr), write32(addr, u32),
//! barrier(), sendEvent(channel, event) u16 and err(msg).

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const busy: u16 = 0x109;
pub const no_data: u16 = 0x10A;
pub const hw_timeout: u16 = 0x203;
pub const null_ptr: u16 = 0x504;

pub const base: usize = 0x4002_0000;
pub const nmi_base: usize = base + 0x80;
pub const nmi_stride: usize = 0x10;
pub const off_nmista: usize = 0x0;
pub const off_nmiset: usize = 0x4;
pub const off_nmiclr: usize = 0x8;
pub const sem_count: u8 = 16;
pub const nmi_unit_count: u8 = 2;
pub const channel_count: u8 = 4;
pub const irq_event_count: u8 = 8;
pub const sem_take_max: u16 = 1024;
const lock: u32 = 1;
const nmi_bit: u32 = 1;

pub const NmiFn = *const fn (ctx: ?*anyopaque, unit: u8) callconv(.c) void;

/// The attached NMI handler; file-static state in the C.
pub const NmiSlot = struct {
    func: ?NmiFn = null,
    ctx: ?*anyopaque = null,
};

/// `ra8_ipc_ring_t`.
pub const Ring = extern struct {
    slots: ?[*]u32 = null,
    head: ?*u32 = null,
    tail: ?*u32 = null,
    capacity: u32 = 0,
    channel: u8 = 0,
    sem_id: u8 = 0,
    notify_id: u8 = 0,
};

pub fn semAddr(id: u8) usize {
    return base + @as(usize, id) * 4;
}

pub fn nmiAddr(unit: u8) usize {
    return nmi_base + @as(usize, unit) * nmi_stride;
}

/// `ra8_ipc_sem_try_take`: the 32-bit read sets LOCK and returns the
/// previous state.
pub fn semTryTake(hw: anytype, id: u8) u16 {
    if (id >= sem_count) return invalid_arg;
    if (hw.read32(semAddr(id)) & lock != 0) return busy;
    hw.barrier();
    return ok;
}

/// `ra8_ipc_sem_take_timeout`: spin up to `max_spins` (capped at 1024).
pub fn semTakeTimeout(hw: anytype, id: u8, max_spins: u16) u16 {
    if (id >= sem_count or max_spins == 0) return invalid_arg;
    const spins = @min(max_spins, sem_take_max);
    var i: u16 = 0;
    while (i < spins) : (i += 1) {
        if (hw.read32(semAddr(id)) & lock == 0) {
            hw.barrier();
            return ok;
        }
    }
    return hw_timeout;
}

/// `ra8_ipc_sem_release`: release barrier, then W1C LOCK.
pub fn semRelease(hw: anytype, id: u8) u16 {
    if (id >= sem_count) return invalid_arg;
    hw.barrier();
    hw.write32(semAddr(id), lock);
    return ok;
}

/// `ra8_ipc_sem_is_locked`: the probe read takes the lock, so give it
/// back when it was free.
pub fn semIsLocked(hw: anytype, id: u8, out_opt: ?*bool) u16 {
    const out = out_opt orelse {
        hw.err("out_locked must not be nullptr");
        return null_ptr;
    };
    if (id >= sem_count) return invalid_arg;
    const prev = hw.read32(semAddr(id)) & lock;
    out.* = prev != 0;
    if (prev == 0) hw.write32(semAddr(id), lock);
    return ok;
}

/// `ra8_ipc_nmi_send`.
pub fn nmiSend(hw: anytype, unit: u8) u16 {
    if (unit >= nmi_unit_count) return invalid_arg;
    hw.write32(nmiAddr(unit) + off_nmiset, nmi_bit);
    return ok;
}

/// `ra8_ipc_nmi_clear`.
pub fn nmiClear(hw: anytype, unit: u8) u16 {
    if (unit >= nmi_unit_count) return invalid_arg;
    hw.write32(nmiAddr(unit) + off_nmiclr, nmi_bit);
    return ok;
}

/// `ra8_ipc_nmi_get_status`.
pub fn nmiGetStatus(hw: anytype, unit: u8, out_opt: ?*bool) u16 {
    const out = out_opt orelse {
        hw.err("out_pending must not be nullptr");
        return null_ptr;
    };
    if (unit >= nmi_unit_count) return invalid_arg;
    out.* = hw.read32(nmiAddr(unit) + off_nmista) & nmi_bit != 0;
    return ok;
}

/// `ra8_ipc_dispatch_nmi`: run the handler only when NMISTA is set,
/// then acknowledge with NMICLR.
pub fn dispatchNmi(hw: anytype, slot: *const NmiSlot, unit: u8) void {
    if (unit >= nmi_unit_count) return;
    if (hw.read32(nmiAddr(unit) + off_nmista) & nmi_bit == 0) return;
    const func = slot.func;
    const ctx = slot.ctx;
    if (func) |f| f(ctx, unit);
    hw.write32(nmiAddr(unit) + off_nmiclr, nmi_bit);
}

fn validate(ring: *const Ring) u16 {
    const cap = ring.capacity;
    if (cap == 0 or (cap & (cap - 1)) != 0) return invalid_arg;
    if (ring.channel >= channel_count) return invalid_arg;
    if (ring.sem_id >= sem_count) return invalid_arg;
    if (ring.notify_id >= irq_event_count) return invalid_arg;
    return ok;
}

/// `ra8_ipc_ring_init`.
pub fn ringInit(hw: anytype, ring_opt: ?*Ring) u16 {
    const ring = ring_opt orelse return nullErr(hw, "ring must not be nullptr");
    if (ring.slots == null) return nullErr(hw, "ring slots must not be nullptr");
    const head = ring.head orelse return nullErr(hw, "ring head must not be nullptr");
    const tail = ring.tail orelse return nullErr(hw, "ring tail must not be nullptr");
    const rc = validate(ring);
    if (rc != ok) return rc;
    head.* = 0;
    tail.* = 0;
    return ok;
}

fn nullErr(hw: anytype, msg: [*:0]const u8) u16 {
    hw.err(msg);
    return null_ptr;
}

/// `ra8_ipc_ring_produce`: append under the semaphore, then raise the
/// notify event on the peer channel.
pub fn ringProduce(hw: anytype, ring_opt: ?*Ring, payload: u32) u16 {
    const ring = ring_opt orelse return nullErr(hw, "ring must not be nullptr");
    const take = semTryTake(hw, ring.sem_id);
    if (take != ok) return take;
    const head = ring.head.?.*;
    const tail = ring.tail.?.*;
    if (head -% tail >= ring.capacity) {
        _ = semRelease(hw, ring.sem_id);
        return busy;
    }
    ring.slots.?[head & (ring.capacity -% 1)] = payload;
    ring.head.?.* = head +% 1;
    const rel = semRelease(hw, ring.sem_id);
    if (rel != ok) return rel;
    return hw.sendEvent(ring.channel, ring.notify_id);
}

/// `ra8_ipc_ring_consume`.
pub fn ringConsume(hw: anytype, ring_opt: ?*Ring, out_opt: ?*u32) u16 {
    const ring = ring_opt orelse return nullErr(hw, "ring must not be nullptr");
    const out = out_opt orelse return nullErr(hw, "out_payload must not be nullptr");
    const take = semTryTake(hw, ring.sem_id);
    if (take != ok) return take;
    const head = ring.head.?.*;
    const tail = ring.tail.?.*;
    if (head == tail) {
        _ = semRelease(hw, ring.sem_id);
        return no_data;
    }
    out.* = ring.slots.?[tail & (ring.capacity -% 1)];
    ring.tail.?.* = tail +% 1;
    return semRelease(hw, ring.sem_id);
}

/// `ra8_ipc_ring_is_empty`.
pub fn ringIsEmpty(hw: anytype, ring_opt: ?*const Ring, out_opt: ?*bool) u16 {
    const ring = ring_opt orelse return nullErr(hw, "ring must not be nullptr");
    const out = out_opt orelse return nullErr(hw, "out_empty must not be nullptr");
    out.* = ring.head.?.* == ring.tail.?.*;
    return ok;
}

/// `ra8_ipc_ring_is_full`.
pub fn ringIsFull(hw: anytype, ring_opt: ?*const Ring, out_opt: ?*bool) u16 {
    const ring = ring_opt orelse return nullErr(hw, "ring must not be nullptr");
    const out = out_opt orelse return nullErr(hw, "out_full must not be nullptr");
    out.* = ring.head.?.* -% ring.tail.?.* >= ring.capacity;
    return ok;
}
