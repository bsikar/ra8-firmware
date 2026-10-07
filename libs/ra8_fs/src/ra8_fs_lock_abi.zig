//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The ra8_fs library lock (RA8FW-724), ported from ra8_fs_fat_lock.c with the
//! C ABI of inc/ra8_fs_seams.h. ra8_fs_set_lock copies the caller's binding
//! (NULL uninstalls it; a binding missing either callback is rejected and the
//! previous one is kept). priv_lock_acquire/priv_lock_release forward to the
//! installed callbacks and are no-ops while none is installed.

pub const ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;

pub const LockFn = *const fn (ctx: ?*anyopaque) callconv(.c) void;

/// Mirror of ra8_fs_lock_t: acquire, release, ctx.
pub const Lock = extern struct {
    acquire: ?LockFn,
    release: ?LockFn,
    ctx: ?*anyopaque,
};

const Installed = struct {
    acquire: LockFn,
    release: LockFn,
    ctx: ?*anyopaque,
};

var installed: ?Installed = null;

pub export fn priv_lock_acquire() void {
    if (installed) |lock| lock.acquire(lock.ctx);
}

pub export fn priv_lock_release() void {
    if (installed) |lock| lock.release(lock.ctx);
}

pub export fn ra8_fs_set_lock(lock: ?*const Lock) u16 {
    const binding = lock orelse {
        installed = null;
        return ok;
    };
    const acquire = binding.acquire orelse return err_invalid_arg;
    const release = binding.release orelse return err_invalid_arg;
    installed = .{ .acquire = acquire, .release = release, .ctx = binding.ctx };
    return ok;
}
