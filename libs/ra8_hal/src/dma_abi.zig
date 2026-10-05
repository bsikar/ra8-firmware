//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the ra8_dma_* channel allocator (RA8FW-741). The channel
//! table lives here; the logic is in internal/dma.zig. The DMAC channel
//! driver (ra8_dmac.c) and ra8_mstp stay C.

const builtin = @import("builtin");
const common = @import("abi_common.zig");
const dma = @import("internal/dma.zig");

const tag = "DMA";

extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;
extern fn ra8_dmac_start(channel: u8, cfg: *const dma.Config) u16;
extern fn ra8_dmac_stop(channel: u8) u16;

var state: dma.State = .{};

const C = struct {
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn dmacStart(_: C, channel: u8, cfg: *const dma.Config) u16 {
        return ra8_dmac_start(channel, cfg);
    }
    pub fn dmacStop(_: C, channel: u8) u16 {
        return ra8_dmac_stop(channel);
    }
    pub fn nullPtr(_: C, msg: [*:0]const u8) u16 {
        common.ra8_log_emit_error(tag, msg);
        return common.k_ra8_err_null_ptr;
    }
    pub fn logError(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_error(tag, msg);
    }
    pub fn logInfo(_: C, msg: [*:0]const u8) void {
        common.ra8_log_emit_info(tag, msg);
    }
    pub fn fail(_: C, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error_val(tag, msg, err);
    }
};

export fn ra8_dma_init() u16 {
    return state.init(C{});
}

export fn ra8_dma_deinit() u16 {
    return state.deinit(C{});
}

export fn ra8_dma_request(req: ?*const dma.Request, out_channel: ?*u8) u16 {
    return state.request(C{}, req, out_channel);
}

export fn ra8_dma_release(channel: u8) u16 {
    return state.release(C{}, channel);
}

export fn ra8_dma_channel_is_busy(channel: u8, out_busy: ?*bool) u16 {
    return state.isBusy(C{}, channel, out_busy);
}

export fn ra8_dma_dispatch_complete(channel: u8) void {
    state.dispatch(channel);
}

/// Host-only, like the C RA8_OFF_TARGET guard: the copy of the request
/// that claimed `channel`, or null when it is out of range or free.
fn fakePeek(channel: u8) callconv(.c) ?*const dma.Request {
    return state.peek(channel);
}

comptime {
    if (builtin.os.tag != .freestanding) @export(&fakePeek, .{ .name = "ra8_dma_fake_peek_request" });
}
