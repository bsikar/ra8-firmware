//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the IPC semaphore, NMI and ring functions in ra8_ipc_sync.h
//! (RA8FW-606). ra8_ipc_send_event stays in ra8_ipc.c.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const ipc = @import("internal/ipc_sem_ring.zig");

const tag = "IPC";

extern fn ra8_ipc_send_event(channel: u8, event_id: u8) u16;

var nmi_slot: ipc.NmiSlot = .{};

const Hw = struct {
    pub fn read32(_: Hw, addr: usize) u32 {
        return @as(*volatile u32, @ptrFromInt(addr)).*;
    }
    pub fn write32(_: Hw, addr: usize, value: u32) void {
        @as(*volatile u32, @ptrFromInt(addr)).* = value;
    }
    /// DMB 0xF on target, nothing off target (ra8_ipc_sync.h).
    pub fn barrier(_: Hw) void {
        if (comptime (builtin.cpu.arch.isArm() or builtin.cpu.arch.isThumb())) asm volatile ("dmb 0xF" ::: "memory");
    }
    pub fn sendEvent(_: Hw, channel: u8, event_id: u8) u16 {
        return ra8_ipc_send_event(channel, event_id);
    }
    pub fn err(_: Hw, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
};

export fn ra8_ipc_sem_try_take(sem_id: u8) u16 {
    return ipc.semTryTake(Hw{}, sem_id);
}

export fn ra8_ipc_sem_take_timeout(sem_id: u8, max_spins: u16) u16 {
    return ipc.semTakeTimeout(Hw{}, sem_id, max_spins);
}

export fn ra8_ipc_sem_release(sem_id: u8) u16 {
    return ipc.semRelease(Hw{}, sem_id);
}

export fn ra8_ipc_sem_is_locked(sem_id: u8, out_locked: ?*bool) u16 {
    return ipc.semIsLocked(Hw{}, sem_id, out_locked);
}

export fn ra8_ipc_nmi_send(unit: u8) u16 {
    return ipc.nmiSend(Hw{}, unit);
}

export fn ra8_ipc_nmi_clear(unit: u8) u16 {
    return ipc.nmiClear(Hw{}, unit);
}

export fn ra8_ipc_nmi_get_status(unit: u8, out_pending: ?*bool) u16 {
    return ipc.nmiGetStatus(Hw{}, unit, out_pending);
}

export fn ra8_ipc_attach_nmi_handler(func: ?ipc.NmiFn, ctx: ?*anyopaque) u16 {
    nmi_slot = .{ .func = func, .ctx = ctx };
    return ipc.ok;
}

export fn ra8_ipc_dispatch_nmi(unit: u8) void {
    ipc.dispatchNmi(Hw{}, &nmi_slot, unit);
}

export fn ra8_ipc_ring_init(ring: ?*ipc.Ring) u16 {
    return ipc.ringInit(Hw{}, ring);
}

export fn ra8_ipc_ring_produce(ring: ?*ipc.Ring, payload: u32) u16 {
    return ipc.ringProduce(Hw{}, ring, payload);
}

export fn ra8_ipc_ring_consume(ring: ?*ipc.Ring, out_payload: ?*u32) u16 {
    return ipc.ringConsume(Hw{}, ring, out_payload);
}

export fn ra8_ipc_ring_is_empty(ring: ?*const ipc.Ring, out_empty: ?*bool) u16 {
    return ipc.ringIsEmpty(Hw{}, ring, out_empty);
}

export fn ra8_ipc_ring_is_full(ring: ?*const ipc.Ring, out_full: ?*bool) u16 {
    return ipc.ringIsFull(Hw{}, ring, out_full);
}
