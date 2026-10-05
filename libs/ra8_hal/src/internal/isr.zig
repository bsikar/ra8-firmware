//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! NVIC + ICU IELSR slot allocator (RA8FW-760). One slot per IELSR route;
//! slot n drives NVIC line n. Generic over `hw`, which provides
//! nvicEnable/nvicDisable/nvicClearPending/nvicSetPriority(n, prio) and
//! ielsrRead/ielsrWrite(slot, value).

pub const ok: u16 = 0;
pub const err_no_mem: u16 = 0x102;
pub const err_invalid_arg: u16 = 0x103;
pub const err_not_found: u16 = 0x106;
pub const err_exists: u16 = 0x10C;

pub const slot_count: u16 = 96;
pub const slot_none: u16 = 0xFFFF;
pub const prio_max: u8 = 15;

pub const iels_mask: u32 = 0x0000_03FF;
pub const ir_bit: u5 = 16;
pub const dtce_mask: u32 = 0x0100_0000;

pub const Handler = *const fn (ctx: ?*anyopaque) callconv(.C) void;

pub const Slot = struct {
    handler: ?Handler = null,
    ctx: ?*anyopaque = null,
    event: u16 = 0,
    priority: u8 = 0,
    in_use: bool = false,
};

pub const Pool = [slot_count]Slot;

pub fn init(pool: *Pool, hw: anytype) void {
    for (pool, 0..) |*s, i| {
        const n: u16 = @intCast(i);
        s.* = .{};
        hw.nvicDisable(n);
        hw.nvicClearPending(n);
        hw.ielsrWrite(n, 0);
    }
}

pub fn findEvent(pool: *const Pool, event: u16) u16 {
    for (pool, 0..) |s, i| {
        if (s.in_use and s.event == event) return @intCast(i);
    }
    return slot_none;
}

fn findFree(pool: *const Pool) u16 {
    for (pool, 0..) |s, i| {
        if (!s.in_use) return @intCast(i);
    }
    return slot_none;
}

/// Caller has already rejected a null handler.
pub fn register(pool: *Pool, hw: anytype, event: u16, handler: Handler, ctx: ?*anyopaque, priority: u8, out_slot: ?*u16) u16 {
    if (priority > prio_max) return err_invalid_arg;
    if (findEvent(pool, event) != slot_none) return err_exists;
    const slot = findFree(pool);
    if (slot == slot_none) return err_no_mem;
    pool[slot] = .{ .handler = handler, .ctx = ctx, .event = event, .priority = priority, .in_use = true };
    hw.ielsrWrite(slot, @as(u32, event) & iels_mask);
    hw.nvicClearPending(slot);
    hw.nvicSetPriority(slot, priority);
    hw.nvicEnable(slot);
    if (out_slot) |o| o.* = slot;
    return ok;
}

pub fn unregister(pool: *Pool, hw: anytype, event: u16) u16 {
    const slot = findEvent(pool, event);
    if (slot == slot_none) return err_not_found;
    hw.nvicDisable(slot);
    hw.ielsrWrite(slot, 0);
    hw.nvicClearPending(slot);
    pool[slot] = .{};
    return ok;
}

/// Clears the IR flag (W0C) before calling the slot's handler.
pub fn dispatch(pool: *const Pool, hw: anytype, slot: u16) void {
    if (slot >= slot_count) return;
    const s = pool[slot];
    hw.ielsrWrite(slot, hw.ielsrRead(slot) & ~(@as(u32, 1) << ir_bit));
    if (s.handler) |h| h(s.ctx);
}

pub fn setPriority(pool: *Pool, hw: anytype, event: u16, priority: u8) u16 {
    if (priority > prio_max) return err_invalid_arg;
    const slot = findEvent(pool, event);
    if (slot == slot_none) return err_not_found;
    pool[slot].priority = priority;
    hw.nvicSetPriority(slot, priority);
    return ok;
}

pub fn setDtc(pool: *const Pool, hw: anytype, slot: u16, enable: bool) u16 {
    if (slot >= slot_count) return err_invalid_arg;
    if (!pool[slot].in_use) return err_not_found;
    const cur = hw.ielsrRead(slot);
    hw.ielsrWrite(slot, if (enable) cur | dtce_mask else cur & ~dtce_mask);
    return ok;
}
