//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DMA channel allocator (RA8FW-741, was ra8_dma.c). Pure: MSTP, the DMAC
//! channel start/stop and logging come in through an `ops` value. The
//! channel table and the initialized flag live in `State`.
//! HUM Ch 11.2.6 "MSTPCRA": DMAC0 and DTC0 share MSTPA22.

pub const ok: u16 = 0;
pub const no_mem: u16 = 0x102;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;
pub const not_initialized: u16 = 0x10F;
pub const hw_init_failed: u16 = 0x201;
pub const hw_error: u16 = 0x204;
pub const null_ptr: u16 = 0x504;

pub const channel_count: u8 = 8;
pub const channel_none: u8 = 0xFF;
/// `k_ra8_mstp_dmac0_dtc0`: register A (0), bit 22.
pub const mstp_dmac0_dtc0: u16 = 22;
pub const width_word: u8 = 2;

pub const CompleteFn = *const fn (ctx: ?*anyopaque) callconv(.c) void;

/// Mirror of `ra8_dma_request_t` (ra8_dma.h).
pub const Request = extern struct {
    src_addr: usize = 0,
    dst_addr: usize = 0,
    count: u16 = 0,
    width: u8 = 0,
    src_inc: bool = false,
    dst_inc: bool = false,
    trigger: u16 = 0,
    on_complete: ?CompleteFn = null,
    ctx: ?*anyopaque = null,
};

/// Mirror of `ra8_dmac_config_t` (ra8_dmac.h).
pub const Config = extern struct {
    src: u32 = 0,
    dst: u32 = 0,
    count: u16 = 0,
    width: u8 = 0,
    src_inc: bool = false,
    dst_inc: bool = false,
    mode: u8 = 0,
    block_count: u16 = 0,
    repeat_area: u8 = 0,
    irq_each: bool = false,
    enable_dtie: bool = false,
};

comptime {
    if (@sizeOf(Config) != 20 or @offsetOf(Config, "block_count") != 14) @compileError("Config must match ra8_dmac_config_t");
    if (@offsetOf(Request, "trigger") != 2 * @sizeOf(usize) + 6) @compileError("Request must match ra8_dma_request_t");
    if (@sizeOf(Request) != 4 * @sizeOf(usize) + 8) @compileError("Request must match ra8_dma_request_t");
}

const Channel = struct {
    on_complete: ?CompleteFn = null,
    ctx: ?*anyopaque = null,
    in_use: bool = false,
};

pub fn validate(req: *const Request) u16 {
    if (req.count == 0) return invalid_arg;
    if (req.width > width_word) return invalid_arg;
    return ok;
}

/// Only the fields ra8_dma.c packed; mode, block count and repeat area
/// stay zero (normal single-shot transfer).
pub fn pack(req: *const Request) Config {
    return .{
        .src = @truncate(req.src_addr),
        .dst = @truncate(req.dst_addr),
        .count = req.count,
        .width = req.width,
        .src_inc = req.src_inc,
        .dst_inc = req.dst_inc,
    };
}

pub const State = struct {
    channels: [channel_count]Channel = [_]Channel{.{}} ** channel_count,
    requests: [channel_count]Request = [_]Request{.{}} ** channel_count,
    initialized: bool = false,

    pub fn init(self: *State, ops: anytype) u16 {
        ops.logInfo("ra8_dma_init");
        const err = ops.mstpEnable(mstp_dmac0_dtc0);
        if (err != ok) {
            ops.fail("mstp enable failed", err);
            return hw_init_failed;
        }
        for (&self.channels) |*ch| ch.* = .{};
        self.initialized = true;
        return ok;
    }

    /// Stops every in-use channel (ignoring stop errors) before dropping
    /// the MSTP reference, so no pending transfer can wedge the block.
    pub fn deinit(self: *State, ops: anytype) u16 {
        for (&self.channels, 0..) |*ch, i| {
            if (!ch.in_use) continue;
            _ = ops.dmacStop(@intCast(i));
            ch.* = .{};
        }
        const err = ops.mstpDisable(mstp_dmac0_dtc0);
        if (err != ok) {
            ops.fail("mstp disable failed", err);
            return hw_error;
        }
        self.initialized = false;
        return ok;
    }

    fn findFree(self: *const State) u8 {
        for (self.channels, 0..) |ch, i| {
            if (!ch.in_use) return @intCast(i);
        }
        return channel_none;
    }

    pub fn request(self: *State, ops: anytype, req: ?*const Request, out: ?*u8) u16 {
        const r = req orelse return ops.nullPtr("request must not be NULL");
        const o = out orelse return ops.nullPtr("out_channel must not be NULL");
        if (!self.initialized) return not_initialized;
        const verr = validate(r);
        if (verr != ok) return verr;
        const ch = self.findFree();
        if (ch == channel_none) {
            ops.logError("no free channel");
            return no_mem;
        }
        const cfg = pack(r);
        const derr = ops.dmacStart(ch, &cfg);
        if (derr != ok) {
            ops.fail("dmac_start failed", derr);
            return hw_error;
        }
        self.channels[ch] = .{ .on_complete = r.on_complete, .ctx = r.ctx, .in_use = true };
        self.requests[ch] = r.*;
        o.* = ch;
        return ok;
    }

    /// ra8_dmac_stop drops one MSTP reference; init holds the root one.
    pub fn release(self: *State, ops: anytype, channel: u8) u16 {
        if (channel >= channel_count) return invalid_arg;
        if (!self.channels[channel].in_use) return invalid_state;
        const err = ops.dmacStop(channel);
        if (err != ok) {
            ops.fail("dmac_stop failed", err);
            return hw_error;
        }
        self.channels[channel] = .{};
        return ok;
    }

    pub fn peek(self: *const State, channel: u8) ?*const Request {
        if (channel >= channel_count) return null;
        if (!self.channels[channel].in_use) return null;
        return &self.requests[channel];
    }

    pub fn isBusy(self: *const State, ops: anytype, channel: u8, out: ?*bool) u16 {
        const o = out orelse return ops.nullPtr("out_busy must not be NULL");
        if (channel >= channel_count) return invalid_arg;
        o.* = self.channels[channel].in_use;
        return ok;
    }

    pub fn dispatch(self: *const State, channel: u8) void {
        if (channel >= channel_count) return;
        const ch = self.channels[channel];
        if (ch.on_complete) |cb| cb(ch.ctx);
    }
};
