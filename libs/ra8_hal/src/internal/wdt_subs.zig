//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! WDT event subscriber table (RA8FW-890, was part of ra8_wdt.c). Slot 0
//! belongs to the legacy attach API; subscribe hands out slots 1..5.
//! Exports live in src/wdt_subs_abi.zig.

pub const max_subs: u8 = 6;
pub const legacy_slot: u8 = 0;

pub const EventFn = *const fn (ctx: ?*anyopaque, status_mask: u16) callconv(.c) void;

pub const Slot = struct {
    func: ?EventFn = null,
    ctx: ?*anyopaque = null,
};

pub const Error = error{ Full, BadSlot, Empty };

pub const Table = struct {
    slots: [max_subs]Slot = @splat(.{}),

    /// A null func clears the legacy slot.
    pub fn attach(self: *Table, func: ?EventFn, ctx: ?*anyopaque) void {
        self.slots[legacy_slot] = .{ .func = func, .ctx = ctx };
    }

    pub fn subscribe(self: *Table, func: EventFn, ctx: ?*anyopaque) Error!u8 {
        var i: u8 = legacy_slot + 1;
        while (i < max_subs) : (i += 1) {
            if (self.slots[i].func == null) {
                self.slots[i] = .{ .func = func, .ctx = ctx };
                return i;
            }
        }
        return error.Full;
    }

    pub fn unsubscribe(self: *Table, slot: u8) Error!void {
        if (slot >= max_subs) return error.BadSlot;
        if (self.slots[slot].func == null) return error.Empty;
        self.slots[slot] = .{};
    }

    pub fn count(self: *const Table) u8 {
        var n: u8 = 0;
        for (self.slots) |s| {
            if (s.func != null) n += 1;
        }
        return n;
    }

    pub fn clear(self: *Table) void {
        self.slots = @splat(.{});
    }

    /// Calls every registered slot in index order with the latched mask.
    pub fn notify(self: *const Table, mask: u16) void {
        for (self.slots) |s| {
            if (s.func) |f| f(s.ctx, mask);
        }
    }
};
