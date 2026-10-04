//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_spi_write_dma / ra8_spi_read_dma (inc/ra8_spi.h,
//! RA8FW-576). ra8_dma_request and the D-cache maintenance stay in C.

const common = @import("abi_common.zig");
const dma = @import("internal/spi_b_dma.zig");

const tag = "SPI_B";

/// Wraps the caller's RX completion so the D-cache is invalidated first.
const RxCtx = struct {
    buf: ?*anyopaque = null,
    len: u16 = 0,
    user_fn: ?dma.CompleteFn = null,
    user_ctx: ?*anyopaque = null,
};

var rx_ctx = [_]RxCtx{.{}} ** dma.channel_count;

extern fn ra8_dma_request(req: *const dma.Request, out_channel: *u8) u16;
extern fn ra8_cache_dcache_line_bytes() u32;
extern fn ra8_cache_dcache_clean_by_addr(addr: ?*const anyopaque, size: u32) u16;
extern fn ra8_cache_dcache_invalidate_by_addr(addr: ?*const anyopaque, size: u32) u16;

fn cacheBytes(len: u32) u32 {
    return dma.roundUp(len, ra8_cache_dcache_line_bytes());
}

fn rxComplete(ctx: ?*anyopaque) callconv(.c) void {
    const rxc: *RxCtx = @ptrCast(@alignCast(ctx orelse return));
    _ = ra8_cache_dcache_invalidate_by_addr(rxc.buf, cacheBytes(rxc.len));
    if (rxc.user_fn) |f| f(rxc.user_ctx);
}

export fn ra8_spi_write_dma(channel: u8, data: ?[*]const u8, len: u16, on_complete: ?dma.CompleteFn, ctx: ?*anyopaque, out_dma_channel: ?*u8) u16 {
    const src = data orelse {
        common.ra8_log_emit_error(tag, "spi_write_dma: data");
        return common.k_ra8_err_null_ptr;
    };
    const out = out_dma_channel orelse {
        common.ra8_log_emit_error(tag, "spi_write_dma: out_dma_channel");
        return common.k_ra8_err_null_ptr;
    };
    if (!dma.argsOk(channel, len)) return common.k_ra8_err_invalid_arg;
    _ = ra8_cache_dcache_clean_by_addr(src, cacheBytes(len));
    const req = dma.txRequest(channel, @intFromPtr(src), len, on_complete, ctx);
    return ra8_dma_request(&req, out);
}

export fn ra8_spi_read_dma(channel: u8, out_buf: ?[*]u8, len: u16, on_complete: ?dma.CompleteFn, ctx: ?*anyopaque, out_dma_channel: ?*u8) u16 {
    const dst = out_buf orelse {
        common.ra8_log_emit_error(tag, "spi_read_dma: out_buf");
        return common.k_ra8_err_null_ptr;
    };
    const out = out_dma_channel orelse {
        common.ra8_log_emit_error(tag, "spi_read_dma: out_dma_channel");
        return common.k_ra8_err_null_ptr;
    };
    if (!dma.argsOk(channel, len)) return common.k_ra8_err_invalid_arg;
    const rxc = &rx_ctx[channel];
    rxc.* = .{ .buf = dst, .len = len, .user_fn = on_complete, .user_ctx = ctx };
    const req = dma.rxRequest(channel, @intFromPtr(dst), len, rxComplete, rxc);
    return ra8_dma_request(&req, out);
}
