//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `fw_if_dma.h`: the `fw_dma` facade exported to C.
//!
//! Everything chip-specific lives behind `fw_dma_iface_t`, so what is left
//! here is small on purpose: reject a malformed request, refuse an unbound
//! handle, and call the binding. Channel indexes are a board fact, so the
//! membrane does not range-check them; the binding owns that and answers
//! `k_ra8_err_not_found` for one it does not carry. The channel description
//! and the transfer shape are what the membrane refuses on its own account:
//! a zeroed `fw_dma_config_t` is the shape a caller gets by forgetting to
//! fill one in, and a DMA unit the pointers are not aligned to is a transfer
//! the chip would silently corrupt.

const std = @import("std");
const core = @import("internal/root.zig");

const Err = core.Err;

/// `fw_dma_direction_t`, `uint8_t`-backed as the header declares it.
pub const Direction = struct {
    pub const none: u8 = 0;
    pub const mem_to_mem: u8 = 1;
    pub const mem_to_per: u8 = 2;
    pub const per_to_mem: u8 = 3;
    pub const count: u8 = 4;
};

/// `fw_dma_width_t`, `uint8_t`-backed as the header declares it. The values
/// are the unit sizes themselves, because they double as the alignment rule.
pub const Width = struct {
    pub const none: u8 = 0;
    pub const one: u8 = 1;
    pub const two: u8 = 2;
    pub const four: u8 = 4;
    pub const count: u8 = 5;
};

/// `fw_dma_config_t`: direction and unit width.
pub const Config = extern struct {
    direction: u8,
    width: u8,
};

/// `fw_dma_iface_t`: the narrow ops struct a chip-and-board binding fills.
pub const Iface = extern struct {
    configure: ?*const fn (?*anyopaque, u8, Config) callconv(.c) Err,
    transfer: ?*const fn (?*anyopaque, u8, ?*const anyopaque, ?*anyopaque, usize) callconv(.c) Err,
};

/// `fw_dma_t`: the caller-owned binding handle.
pub const Dma = extern struct {
    iface: ?*const Iface,
    ctx: ?*anyopaque,
    bound: bool,
};

// The header pins the config's numeric width and the handle's flag to plain C
// uint8_t and C bool; both are load-bearing for the layouts above.
comptime {
    std.debug.assert(@sizeOf(Config) == 2);
    std.debug.assert(@sizeOf(bool) == 1);
}

/// Whether a channel description names fields this contract enumerates. The
/// zeroed shape is the unconfigured config, refused like any out-of-range
/// value.
fn cfgValid(cfg: Config) bool {
    const direction_ok = cfg.direction != Direction.none and cfg.direction < Direction.count;
    const width_ok = cfg.width == Width.one or cfg.width == Width.two or cfg.width == Width.four;
    return direction_ok and width_ok;
}

/// Common entry guard: bound handle.
fn check(dma: ?*const Dma) Err {
    const handle = dma orelse return core.err_invalid_arg;
    if (!handle.bound) return core.err_not_initialized;
    return core.ok;
}

pub export fn fw_dma_bind(dma: ?*Dma, iface: ?*const Iface, ctx: ?*anyopaque) callconv(.c) Err {
    const handle = dma orelse return core.err_invalid_arg;
    const ops = iface orelse return core.err_invalid_arg;
    // A NULL op is a malformed binding, not a declined capability: a binding
    // that cannot move data says so from transfer, not by leaving it unset.
    if (ops.configure == null or ops.transfer == null) {
        return core.err_invalid_arg;
    }
    handle.iface = ops;
    handle.ctx = ctx;
    handle.bound = true;
    return core.ok;
}

pub export fn fw_dma_configure(dma: ?*const Dma, chan: u8, cfg: Config) callconv(.c) Err {
    if (!cfgValid(cfg)) return core.err_invalid_arg;

    const guard = check(dma);
    if (guard != core.ok) return guard;

    const handle = dma.?;
    return handle.iface.?.configure.?(handle.ctx, chan, cfg);
}

pub export fn fw_dma_transfer(dma: ?*const Dma, chan: u8, src: ?*const anyopaque, dst: ?*anyopaque, len: usize) callconv(.c) Err {
    // A zero-byte transfer asks for nothing; it is satisfied without arming
    // the channel.
    if (len == 0) return core.ok;
    // Both ends of a real transfer must exist.
    if (src == null or dst == null) return core.err_invalid_arg;

    const guard = check(dma);
    if (guard != core.ok) return guard;

    // Alignment to the channel's configured width is the binding's contract:
    // only the binding knows which bus masters the channel serves, so a
    // transfer it cannot carry comes back as the binding's refusal, never as
    // a corrupted tail.
    const handle = dma.?;
    return handle.iface.?.transfer.?(handle.ctx, chan, src.?, dst.?, len);
}
