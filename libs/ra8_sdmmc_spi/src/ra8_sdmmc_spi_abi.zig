//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for the SPI-mode SD card driver core (`ra8_sdmmc_spi`),
//! replacing `src/ra8_sdmmc_spi.c`. This half owns the single driver-state
//! object and every low-level transport, framing and identification helper
//! that `src/ra8_sdmmc_spi_internal.h` publishes to the block-I/O
//! translation unit, which is still C and still calls straight into these
//! symbols.
//!
//! Nothing here caches a callback: every byte still goes through
//! `g_sdmmc_spi_state.transport`, so a suite that swaps the transport
//! between calls sees the swap immediately, exactly as under the C.

const std = @import("std");
const core = @import("internal/root.zig");

pub const Transport = core.Transport;
pub const State = core.State;
pub const CardType = core.CardType;

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Transport) == ptr * 4);
    std.debug.assert(@offsetOf(State, "transport") == 0);
    std.debug.assert(@offsetOf(State, "card_type") == ptr * 4);
    std.debug.assert(@sizeOf(State) == std.mem.alignForward(
        usize,
        @offsetOf(State, "initialized") + 1,
        ptr,
    ));
}

extern fn ra8_log_emit_error(tag: ?[*:0]const u8, message: ?[*:0]const u8) void;
extern fn ra8_log_emit_error_val(tag: ?[*:0]const u8, message: ?[*:0]const u8, value: u32) void;

const tag: [*:0]const u8 = "SDSPI";

/// `RA8_RETURN_ON_ERROR`: the message line then the numeric line.
fn logError(message: [*:0]const u8, code: u16) void {
    ra8_log_emit_error(tag, message);
    ra8_log_emit_error_val(tag, "Error", @as(u32, code));
}

/// The module's one external definition of the shared driver state.
pub export var g_sdmmc_spi_state: State = .{};

// ---------------------------------------------------------------------------
// CRC helpers (public API, exported for unit-test coverage)
// ---------------------------------------------------------------------------

pub export fn ra8_sdmmc_spi_crc7(data: ?[*]const u8, len: u32) callconv(.c) u8 {
    return core.crc7(data, len);
}

pub export fn ra8_sdmmc_spi_crc16(data: ?[*]const u8, len: u32) callconv(.c) u16 {
    return core.crc16(data, len);
}

// ---------------------------------------------------------------------------
// Low-level byte exchange
// ---------------------------------------------------------------------------

fn xfer(tx: ?[*]const u8, rx: ?[*]u8, len: u32) u16 {
    const f = g_sdmmc_spi_state.transport.xfer orelse return core.err.null_ptr;
    return f(g_sdmmc_spi_state.transport.ctx, tx, rx, len);
}

fn chipSelect(asserted: bool) u16 {
    const f = g_sdmmc_spi_state.transport.cs orelse return core.err.null_ptr;
    return f(g_sdmmc_spi_state.transport.ctx, asserted);
}

pub export fn priv_sdmmc_spi_xfer_one(tx: u8, rx: ?[*]u8) callconv(.c) u16 {
    const tx_buf = [1]u8{tx};
    var rx_buf = [1]u8{0};
    const rc = xfer(&tx_buf, &rx_buf, 1);
    if (rc != core.err.ok) return rc;
    if (rx) |out| out[0] = rx_buf[0];
    return core.err.ok;
}

pub export fn priv_sdmmc_spi_send_idle(n: u32) callconv(.c) u16 {
    var byte: u8 = 0;
    var i: u32 = 0;
    while (i < n) : (i += 1) {
        const rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&byte));
        if (rc != core.err.ok) return rc;
    }
    return core.err.ok;
}

pub export fn priv_sdmmc_spi_cs_assert() callconv(.c) u16 {
    const rc = chipSelect(true);
    if (rc != core.err.ok) return rc;
    return priv_sdmmc_spi_send_idle(1);
}

pub export fn priv_sdmmc_spi_cs_release() callconv(.c) u16 {
    const rc = chipSelect(false);
    if (rc != core.err.ok) return rc;
    return priv_sdmmc_spi_send_idle(1);
}

// ---------------------------------------------------------------------------
// Command framing and responses
// ---------------------------------------------------------------------------

/// Poll bounded idle bytes until the card presents an R1 token.
fn readR1(out_r1: ?[*]u8) u16 {
    var i: u32 = 0;
    while (i < core.proto.max_r1_wait_bytes) : (i += 1) {
        var byte: u8 = 0;
        const rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&byte));
        if (rc != core.err.ok) return rc;
        if (core.isR1(byte)) {
            if (out_r1) |out| out[0] = byte;
            return core.err.ok;
        }
    }
    return core.err.hw_timeout;
}

pub export fn priv_sdmmc_spi_send_command(command: u8, arg: u32, out_r1: ?[*]u8) callconv(.c) u16 {
    var frame: [6]u8 = undefined;
    core.buildFrame(command, arg, &frame);
    var rx_dummy: [6]u8 = undefined;
    const rc = xfer(&frame, &rx_dummy, core.cmd_frame_bytes);
    if (rc != core.err.ok) return rc;
    return readR1(out_r1);
}

pub export fn priv_sdmmc_spi_send_acmd(acmd: u8, arg: u32, out_r1: ?[*]u8) callconv(.c) u16 {
    var r1: u8 = 0;
    const rc = priv_sdmmc_spi_send_command(core.cmd.app_cmd, 0, @ptrCast(&r1));
    if (rc != core.err.ok) return rc;
    return priv_sdmmc_spi_send_command(acmd, arg, out_r1);
}

/// CMD12 cannot reuse `send_command`: one stuff byte follows the frame on an
/// interrupted read stream and its value is undefined, so a bit-7-clear
/// garbage byte would be mistaken for R1. Discard that byte first.
pub export fn priv_sdmmc_spi_send_stop_transmission(out_r1: ?[*]u8) callconv(.c) u16 {
    var frame: [6]u8 = undefined;
    core.buildFrame(core.cmd.stop_transmission, 0, &frame);
    var rx_dummy: [6]u8 = undefined;
    var rc = xfer(&frame, &rx_dummy, core.cmd_frame_bytes);
    if (rc != core.err.ok) return rc;
    rc = priv_sdmmc_spi_send_idle(1);
    if (rc != core.err.ok) return rc;
    return readR1(out_r1);
}

/// Read the four-byte tail of an R3 or R7 response. A transport that refuses
/// the bulk transfer is retried byte by byte, which keeps single-byte-only
/// transports working; the C had the same fallback.
fn readR3OrR7Tail(out_word: *u32) u16 {
    var bytes = [4]u8{ 0, 0, 0, 0 };
    var rc = xfer(null, &bytes, 4);
    if (rc != core.err.ok) {
        var i: u32 = 0;
        while (i < 4) : (i += 1) {
            rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&bytes[i]));
            if (rc != core.err.ok) return rc;
        }
    }
    out_word.* = core.tailWord(bytes);
    return core.err.ok;
}

// ---------------------------------------------------------------------------
// Bounded waits
// ---------------------------------------------------------------------------

pub export fn priv_sdmmc_spi_wait_data_token() callconv(.c) u16 {
    var i: u32 = 0;
    while (i < core.proto.max_data_token_polls) : (i += 1) {
        var byte: u8 = 0;
        const rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&byte));
        if (rc != core.err.ok) return rc;
        if (byte == core.token.data_start_single) return core.err.ok;
    }
    return core.err.hw_timeout;
}

pub export fn priv_sdmmc_spi_wait_not_busy_bounded(max_polls: u32) callconv(.c) u16 {
    var i: u32 = 0;
    while (i < max_polls) : (i += 1) {
        var byte: u8 = 0;
        const rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&byte));
        if (rc != core.err.ok) return rc;
        if (byte == core.token.idle) return core.err.ok;
    }
    return core.err.hw_timeout;
}

pub export fn priv_sdmmc_spi_wait_not_busy() callconv(.c) u16 {
    return priv_sdmmc_spi_wait_not_busy_bounded(core.proto.max_busy_poll_bytes);
}

// ---------------------------------------------------------------------------
// Transport validation
// ---------------------------------------------------------------------------

pub export fn priv_sdmmc_spi_validate_transport(transport: ?*const Transport) callconv(.c) u16 {
    return core.validateTransport(transport);
}

// ---------------------------------------------------------------------------
// Identification sequence
// ---------------------------------------------------------------------------

/// Best-effort unstick of a card left mid-stream: release, clock idle, then
/// stop-token, flush, CMD12 and release again. Every step's result is
/// deliberately discarded, as in the C; the caller retries the probe.
fn recoverStuckCard() void {
    _ = chipSelect(false);
    _ = priv_sdmmc_spi_send_idle(core.proto.recover_idle_bytes);

    if (chipSelect(true) != core.err.ok) return;
    _ = priv_sdmmc_spi_xfer_one(core.token.stop_multi, null);
    _ = priv_sdmmc_spi_send_idle(core.proto.recover_flush_bytes);
    _ = priv_sdmmc_spi_wait_not_busy();

    var r1: u8 = 0;
    _ = priv_sdmmc_spi_send_command(core.cmd.stop_transmission, 0, @ptrCast(&r1));
    _ = priv_sdmmc_spi_wait_not_busy();
    _ = chipSelect(false);

    _ = priv_sdmmc_spi_send_idle(core.proto.recover_idle_bytes);
}

fn wakeCard() u16 {
    const rc = chipSelect(false);
    if (rc != core.err.ok) return rc;
    return priv_sdmmc_spi_send_idle(core.wakeIdleBytes());
}

fn sendCmd0() u16 {
    var rc = priv_sdmmc_spi_cs_assert();
    if (rc != core.err.ok) return rc;
    var r1: u8 = 0;
    rc = priv_sdmmc_spi_send_command(core.cmd.go_idle_state, 0, @ptrCast(&r1));
    _ = priv_sdmmc_spi_cs_release();
    if (rc != core.err.ok) return rc;
    if (r1 != core.r1.idle_state) return core.err.protocol_error;
    return core.err.ok;
}

fn sendCmd8(out_is_v2: *bool) u16 {
    var rc = priv_sdmmc_spi_cs_assert();
    if (rc != core.err.ok) return rc;
    var r1: u8 = 0;
    rc = priv_sdmmc_spi_send_command(
        core.cmd.send_if_cond,
        core.proto.cmd8_arg_check_pattern,
        @ptrCast(&r1),
    );
    if (rc != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return rc;
    }
    if (core.cmd8SaysLegacy(r1)) {
        out_is_v2.* = false;
        _ = priv_sdmmc_spi_cs_release();
        return core.err.ok;
    }
    var echo: u32 = 0;
    rc = readR3OrR7Tail(&echo);
    _ = priv_sdmmc_spi_cs_release();
    if (rc != core.err.ok) return rc;
    if (!core.echoMatches(echo)) return core.err.protocol_error;
    out_is_v2.* = true;
    return core.err.ok;
}

fn acmd41Loop(is_v2: bool) u16 {
    const arg: u32 = if (is_v2) core.proto.acmd41_arg_hcs else 0;
    var i: u32 = 0;
    while (i < core.proto.max_acmd41_attempts) : (i += 1) {
        var rc = priv_sdmmc_spi_cs_assert();
        if (rc != core.err.ok) return rc;
        var r1: u8 = 0;
        rc = priv_sdmmc_spi_send_acmd(core.cmd.acmd_sd_send_op_cond, arg, @ptrCast(&r1));
        _ = priv_sdmmc_spi_cs_release();
        if (rc != core.err.ok) return rc;
        if (core.acmd41Done(r1)) return core.err.ok;
    }
    return core.err.hw_init_failed;
}

fn readOcr(out_is_hc: *bool) u16 {
    var rc = priv_sdmmc_spi_cs_assert();
    if (rc != core.err.ok) return rc;
    var r1: u8 = 0;
    rc = priv_sdmmc_spi_send_command(core.cmd.read_ocr, 0, @ptrCast(&r1));
    if (rc != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return rc;
    }
    var ocr: u32 = 0;
    rc = readR3OrR7Tail(&ocr);
    _ = priv_sdmmc_spi_cs_release();
    if (rc != core.err.ok) return rc;
    out_is_hc.* = core.ocrIsHighCapacity(ocr);
    return core.err.ok;
}

fn readCsd(out_blocks: *u32) u16 {
    var rc = priv_sdmmc_spi_cs_assert();
    if (rc != core.err.ok) return rc;
    var r1: u8 = 0;
    rc = priv_sdmmc_spi_send_command(core.cmd.send_csd, 0, @ptrCast(&r1));
    if (rc != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return rc;
    }
    if (r1 != 0) {
        _ = priv_sdmmc_spi_cs_release();
        return core.err.protocol_error;
    }
    rc = priv_sdmmc_spi_wait_data_token();
    if (rc != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return rc;
    }
    var csd = [_]u8{0} ** 16;
    var i: u32 = 0;
    while (i < core.csd_response_len) : (i += 1) {
        rc = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&csd[i]));
        if (rc != core.err.ok) {
            _ = priv_sdmmc_spi_cs_release();
            return rc;
        }
    }
    // The two CRC16 bytes are clocked and dropped, as in the C.
    var crc_bytes = [2]u8{ 0, 0 };
    _ = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&crc_bytes[0]));
    _ = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&crc_bytes[1]));
    _ = priv_sdmmc_spi_cs_release();
    out_blocks.* = core.csdToBlocks(&csd);
    if (out_blocks.* == 0) return core.err.protocol_error;
    return core.err.ok;
}

fn setBlockLen() u16 {
    var rc = priv_sdmmc_spi_cs_assert();
    if (rc != core.err.ok) return rc;
    var r1: u8 = 0;
    rc = priv_sdmmc_spi_send_command(core.cmd.set_blocklen, core.block_size, @ptrCast(&r1));
    _ = priv_sdmmc_spi_cs_release();
    if (rc != core.err.ok) return rc;
    if (r1 != 0) return core.err.protocol_error;
    return core.err.ok;
}

fn probeCard(out_is_v2: *bool, out_is_hc: *bool) u16 {
    var rc = wakeCard();
    if (rc == core.err.ok) rc = sendCmd0();
    var attempt: u32 = 0;
    while (rc != core.err.ok and attempt < core.proto.max_recover_attempts) : (attempt += 1) {
        recoverStuckCard();
        rc = wakeCard();
        if (rc == core.err.ok) rc = sendCmd0();
    }
    if (rc != core.err.ok) return rc;
    rc = sendCmd8(out_is_v2);
    if (rc != core.err.ok) return rc;
    rc = acmd41Loop(out_is_v2.*);
    if (rc != core.err.ok) return rc;
    out_is_hc.* = false;
    if (out_is_v2.*) rc = readOcr(out_is_hc);
    return rc;
}

/// CMD0 / CMD8 / ACMD41 / CMD58 / CMD9 / CMD16, then publish the card class
/// and capacity. Only CMD9 and CMD16 log on failure, as in the C.
pub export fn priv_sdmmc_spi_run_init_sequence() callconv(.c) u16 {
    var is_v2 = false;
    var is_hc = false;
    var rc = probeCard(&is_v2, &is_hc);
    if (rc != core.err.ok) return rc;

    var blocks: u32 = 0;
    rc = readCsd(&blocks);
    if (rc != core.err.ok) {
        logError("CMD9 SEND_CSD", rc);
        return rc;
    }
    rc = setBlockLen();
    if (rc != core.err.ok) {
        logError("CMD16 SET_BLOCKLEN", rc);
        return rc;
    }
    g_sdmmc_spi_state.card_type = @intFromEnum(core.classifyCard(is_v2, is_hc));
    g_sdmmc_spi_state.capacity_blocks = blocks;
    return core.err.ok;
}

// ===========================================================================
// Block I/O and the SCI transport factory (was `src/ra8_sdmmc_spi_io.c`)
// ===========================================================================

pub const SciPins = core.SciPins;
pub const SciSpiCfg = core.SciSpiCfg;
pub const FsBackend = core.FsBackend;

extern fn ra8_sci_spi_init(channel: u8, cfg: ?*const core.SciSpiCfg) u16;
extern fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) u16;
extern fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) u16;
extern fn ra8_gpio_output_init(pin: u16, init_level: u8) u16;
extern fn ra8_gpio_write(pin: u16, lvl: u8) u16;
extern fn ra8_pfs_route_peripheral(pin: u16, sel: u8, owner: ?[*:0]const u8) u16;

// ---------------------------------------------------------------------------
// Convenience SCI Simple-SPI transport factory (Pmod SD on EK-RA8D2)
// ---------------------------------------------------------------------------

/// Single-shot bus context backing the factory's transport callbacks. Only
/// one SD bus may be brought up through the factory at a time, exactly as
/// under the C.
var s_sci_ctx: core.SciBusCtx = .{};

fn sciTransportSetClock(ctx: ?*anyopaque, hz: u32) callconv(.c) u16 {
    const raw = ctx orelse {
        ra8_log_emit_error(tag, "ctx");
        return core.err.null_ptr;
    };
    const c: *const core.SciBusCtx = @ptrCast(@alignCast(raw));
    return ra8_sci_spi_set_clock(c.channel, hz, c.pclka_hz);
}

fn sciTransportCs(ctx: ?*anyopaque, asserted: bool) callconv(.c) u16 {
    const raw = ctx orelse {
        ra8_log_emit_error(tag, "ctx");
        return core.err.null_ptr;
    };
    const c: *const core.SciBusCtx = @ptrCast(@alignCast(raw));
    return ra8_gpio_write(c.cs, if (asserted) core.level.low else core.level.high);
}

fn sciTransportXfer(ctx: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u32) callconv(.c) u16 {
    const raw = ctx orelse {
        ra8_log_emit_error(tag, "ctx");
        return core.err.null_ptr;
    };
    const c: *const core.SciBusCtx = @ptrCast(@alignCast(raw));
    return ra8_sci_spi_xfer(c.channel, tx, rx, len);
}

/// Route the four Pmod SPI pins and bring up the SCI channel at the SD
/// power-on clock. Every pin is routed in order and the first failure
/// returns, leaving the remaining pins untouched.
fn sciTransportBringup(channel: u8, pclk_hz: u32, pins: *const SciPins) u16 {
    var code = ra8_pfs_route_peripheral(pins.sck, core.psel_sci_async, "sdspi.sck");
    if (code != core.err.ok) return code;
    code = ra8_pfs_route_peripheral(pins.cipo, core.psel_sci_async, "sdspi.cipo");
    if (code != core.err.ok) return code;
    code = ra8_pfs_route_peripheral(pins.copi, core.psel_sci_async, "sdspi.copi");
    if (code != core.err.ok) return code;
    code = ra8_gpio_output_init(pins.cs, core.level.high);
    if (code != core.err.ok) return code;
    const spi_cfg: core.SciSpiCfg = .{
        .baud_hz = core.clock_init_hz,
        .pclk_hz = pclk_hz,
        .mode = core.spi_mode_0,
        .lsb_first = false,
    };
    return ra8_sci_spi_init(channel, &spi_cfg);
}

pub export fn ra8_sdmmc_spi_transport_sci(
    sci_channel: u8,
    pclk_hz: u32,
    pins: ?*const SciPins,
    out: ?*Transport,
) callconv(.c) u16 {
    const p = pins orelse {
        ra8_log_emit_error(tag, "pins");
        return core.err.null_ptr;
    };
    const o = out orelse {
        ra8_log_emit_error(tag, "out");
        return core.err.null_ptr;
    };
    if (!core.factoryPclkOk(pclk_hz)) return core.err.invalid_arg;

    const code = sciTransportBringup(sci_channel, pclk_hz, p);
    if (code != core.err.ok) return code;

    s_sci_ctx.channel = sci_channel;
    s_sci_ctx.pclka_hz = pclk_hz;
    s_sci_ctx.cs = p.cs;

    o.set_clock = &sciTransportSetClock;
    o.cs = &sciTransportCs;
    o.xfer = &sciTransportXfer;
    o.ctx = @ptrCast(&s_sci_ctx);
    return core.err.ok;
}

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

/// Adopt the transport, clear the probe results and drop the bus to the
/// mandatory init clock. A second init without a deinit is refused.
fn prepareInit(transport: *const Transport) u16 {
    if (g_sdmmc_spi_state.initialized) {
        ra8_log_emit_error(tag, "already initialized");
        return core.err.invalid_state;
    }
    g_sdmmc_spi_state.transport = transport.*;
    g_sdmmc_spi_state.card_type = @intFromEnum(CardType.unknown);
    g_sdmmc_spi_state.capacity_blocks = 0;
    const f = g_sdmmc_spi_state.transport.set_clock orelse return core.err.null_ptr;
    return f(g_sdmmc_spi_state.transport.ctx, core.clock_init_hz);
}

/// Bump the bus to the data clock and only then publish the handle as
/// initialised: a failed retune leaves the driver closed.
fn finalizeInit() u16 {
    const f = g_sdmmc_spi_state.transport.set_clock orelse return core.err.null_ptr;
    const code = f(g_sdmmc_spi_state.transport.ctx, core.clock_data_hz);
    if (code != core.err.ok) return code;
    g_sdmmc_spi_state.initialized = true;
    return core.err.ok;
}

pub export fn ra8_sdmmc_spi_init(transport: ?*const Transport) callconv(.c) u16 {
    var code = core.validateTransport(transport);
    if (code != core.err.ok) {
        logError("transport invalid", code);
        return code;
    }
    code = prepareInit(transport.?);
    if (code != core.err.ok) {
        logError("prepare init", code);
        return code;
    }
    code = priv_sdmmc_spi_run_init_sequence();
    if (code != core.err.ok) {
        logError("SD init sequence", code);
        return code;
    }
    code = finalizeInit();
    if (code != core.err.ok) {
        logError("finalize init", code);
        return code;
    }
    return core.err.ok;
}

pub export fn ra8_sdmmc_spi_deinit() callconv(.c) u16 {
    g_sdmmc_spi_state.initialized = false;
    g_sdmmc_spi_state.card_type = @intFromEnum(CardType.unknown);
    g_sdmmc_spi_state.capacity_blocks = 0;
    return core.err.ok;
}

// ---------------------------------------------------------------------------
// Shared command / data helpers
// ---------------------------------------------------------------------------

fn lbaArg(lba: u32) u32 {
    return core.lbaToArg(g_sdmmc_spi_state.card_type, lba);
}

/// One command inside its own CS frame that must answer a clean R1.
fn cmdRequireReady(command: u8, arg: u32) u16 {
    var code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) return code;
    var r1: u8 = 0;
    code = priv_sdmmc_spi_send_command(command, arg, @ptrCast(&r1));
    _ = priv_sdmmc_spi_cs_release();
    if (code != core.err.ok) return code;
    if (r1 != 0) return core.err.protocol_error;
    return core.err.ok;
}

/// CMD32/CMD33 to set the range, then CMD38 and a long busy wait: a bulk
/// erase can hold the card busy for seconds.
fn eraseRange(lba: u32, count: u32) u16 {
    var code = cmdRequireReady(core.cmd.erase_wr_blk_start, lbaArg(lba));
    if (code != core.err.ok) return code;
    code = cmdRequireReady(core.cmd.erase_wr_blk_end, lbaArg(core.eraseEndLba(lba, count)));
    if (code != core.err.ok) return code;

    code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) return code;
    var r1: u8 = 0;
    code = priv_sdmmc_spi_send_command(core.cmd.erase, 0, @ptrCast(&r1));
    if (code != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return code;
    }
    if (r1 != 0) {
        _ = priv_sdmmc_spi_cs_release();
        return core.err.protocol_error;
    }
    code = priv_sdmmc_spi_wait_not_busy_bounded(core.proto.max_erase_poll_bytes);
    _ = priv_sdmmc_spi_cs_release();
    return code;
}

fn readBlockPayload(buf: [*]u8) u16 {
    var i: u32 = 0;
    while (i < core.block_size) : (i += 1) {
        const code = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&buf[i]));
        if (code != core.err.ok) return code;
    }
    return core.err.ok;
}

/// Read the two trailing CRC bytes and check them against the payload. The
/// two exchanges are deliberately unchecked, exactly as the C left them: a
/// transport fault shows up as a CRC mismatch.
fn readBlockCrcCheck(buf: [*]const u8) u16 {
    var crc_hi: u8 = 0;
    var crc_lo: u8 = 0;
    _ = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&crc_hi));
    _ = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&crc_lo));
    const expected = core.crc16(buf, core.block_size);
    if (expected != core.crcFromBytes(crc_hi, crc_lo)) return core.err.crc_mismatch;
    return core.err.ok;
}

fn readDataPhase(lba: u32, buf: [*]u8) u16 {
    var r1: u8 = 0;
    var code = priv_sdmmc_spi_send_command(core.cmd.read_single_block, lbaArg(lba), @ptrCast(&r1));
    if (code != core.err.ok) return code;
    if (r1 != 0) return core.err.protocol_error;
    code = priv_sdmmc_spi_wait_data_token();
    if (code != core.err.ok) return code;
    code = readBlockPayload(buf);
    if (code != core.err.ok) return code;
    return readBlockCrcCheck(buf);
}

// ---------------------------------------------------------------------------
// Read paths
// ---------------------------------------------------------------------------

pub export fn ra8_sdmmc_spi_read_block(lba: u32, buf: ?[*]u8) callconv(.c) u16 {
    const dst = buf orelse {
        ra8_log_emit_error(tag, "buf is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    if (!core.singleBlockInRange(g_sdmmc_spi_state.capacity_blocks, lba)) {
        return core.err.out_of_range;
    }
    const code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) {
        logError("cs assert", code);
        return code;
    }
    const result = readDataPhase(lba, dst);
    _ = priv_sdmmc_spi_cs_release();
    return result;
}

/// Drain `count` streamed blocks (token + payload + CRC) of an open CMD18.
fn readMultiStream(buf: [*]u8, count: u32) u16 {
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const block = buf + (@as(usize, i) * core.block_size);
        var code = priv_sdmmc_spi_wait_data_token();
        if (code != core.err.ok) return code;
        code = readBlockPayload(block);
        if (code != core.err.ok) return code;
        code = readBlockCrcCheck(block);
        if (code != core.err.ok) return code;
    }
    return core.err.ok;
}

fn readMultiStop() u16 {
    var r1: u8 = 0;
    const code = priv_sdmmc_spi_send_stop_transmission(@ptrCast(&r1));
    if (code != core.err.ok) return code;
    if (r1 != 0) {
        _ = priv_sdmmc_spi_wait_not_busy();
        return core.err.protocol_error;
    }
    return priv_sdmmc_spi_wait_not_busy();
}

pub export fn ra8_sdmmc_spi_read_blocks(lba: u32, buf: ?[*]u8, count: u32) callconv(.c) u16 {
    const dst = buf orelse {
        ra8_log_emit_error(tag, "buf is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    if (count == 0) return core.err.ok;
    if (!core.multiBlockInRange(g_sdmmc_spi_state.capacity_blocks, lba, count)) {
        return core.err.out_of_range;
    }
    if (count == 1) return ra8_sdmmc_spi_read_block(lba, dst);

    var code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) {
        logError("cs assert", code);
        return code;
    }
    var r1: u8 = 0;
    code = priv_sdmmc_spi_send_command(core.cmd.read_multi_block, lbaArg(lba), @ptrCast(&r1));
    if (code != core.err.ok or r1 != 0) {
        _ = priv_sdmmc_spi_cs_release();
        return if (code != core.err.ok) code else core.err.protocol_error;
    }
    code = readMultiStream(dst, count);
    // CMD12 runs on the success AND the abort path: the card keeps streaming
    // read data until it sees STOP_TRANSMISSION, so it must leave the data
    // state before CS is released or the next command collides with the
    // still-open stream. A stream error takes precedence over a stop error.
    const stop_code = readMultiStop();
    _ = priv_sdmmc_spi_cs_release();
    if (code != core.err.ok) return code;
    return stop_code;
}

// ---------------------------------------------------------------------------
// Write paths
// ---------------------------------------------------------------------------

/// One data block: N_WR pad, start token, payload, CRC16, then the
/// data-response token and the programming busy wait.
fn writeDataBlock(buf: [*]const u8, start_token: u8) u16 {
    var code = priv_sdmmc_spi_send_idle(1); // N_WR pad (spec >= 1 byte).
    if (code != core.err.ok) return code;
    code = priv_sdmmc_spi_xfer_one(start_token, null);
    if (code != core.err.ok) return code;

    const f = g_sdmmc_spi_state.transport.xfer;
    code = if (f) |xf|
        xf(g_sdmmc_spi_state.transport.ctx, buf, null, core.block_size)
    else
        core.err.null_ptr;
    if (code != core.err.ok) {
        // Some transports require non-NULL rx -- fall back to per-byte.
        var i: u32 = 0;
        while (i < core.block_size) : (i += 1) {
            var dummy: u8 = 0;
            code = priv_sdmmc_spi_xfer_one(buf[i], @ptrCast(&dummy));
            if (code != core.err.ok) return code;
        }
    }

    const crc = core.crcBytes(core.crc16(buf, core.block_size));
    _ = priv_sdmmc_spi_xfer_one(crc[0], null);
    _ = priv_sdmmc_spi_xfer_one(crc[1], null);

    var response: u8 = 0;
    code = priv_sdmmc_spi_xfer_one(core.token.idle, @ptrCast(&response));
    if (code != core.err.ok) return code;
    if (!core.dataResponseAccepted(response)) {
        _ = priv_sdmmc_spi_wait_not_busy();
        return core.err.protocol_error;
    }
    return priv_sdmmc_spi_wait_not_busy();
}

pub export fn ra8_sdmmc_spi_write_block(lba: u32, buf: ?[*]const u8) callconv(.c) u16 {
    const src = buf orelse {
        ra8_log_emit_error(tag, "buf is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    if (!core.singleBlockInRange(g_sdmmc_spi_state.capacity_blocks, lba)) {
        return core.err.out_of_range;
    }
    var code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) {
        logError("cs assert", code);
        return code;
    }
    var r1: u8 = 0;
    code = priv_sdmmc_spi_send_command(core.cmd.write_single_block, lbaArg(lba), @ptrCast(&r1));
    if (code != core.err.ok) {
        _ = priv_sdmmc_spi_cs_release();
        return code;
    }
    if (r1 != 0) {
        _ = priv_sdmmc_spi_cs_release();
        return core.err.protocol_error;
    }
    const result = writeDataBlock(src, core.token.data_start_single);
    _ = priv_sdmmc_spi_cs_release();
    return result;
}

/// Stream `count` blocks with the multi-block start token, then the
/// stop-tran token and the final busy wait.
fn writeMultiStream(buf: [*]const u8, count: u32) u16 {
    var i: u32 = 0;
    while (i < count) : (i += 1) {
        const code = writeDataBlock(
            buf + (@as(usize, i) * core.block_size),
            core.token.data_start_multi,
        );
        if (code != core.err.ok) return code;
    }
    _ = priv_sdmmc_spi_send_idle(1);
    _ = priv_sdmmc_spi_xfer_one(core.token.stop_multi, null);
    _ = priv_sdmmc_spi_send_idle(1);
    return priv_sdmmc_spi_wait_not_busy();
}

pub export fn ra8_sdmmc_spi_write_blocks(lba: u32, buf: ?[*]const u8, count: u32) callconv(.c) u16 {
    const src = buf orelse {
        ra8_log_emit_error(tag, "buf is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    if (count == 0) return core.err.ok;
    if (!core.multiBlockInRange(g_sdmmc_spi_state.capacity_blocks, lba, count)) {
        return core.err.out_of_range;
    }
    if (count == 1) return ra8_sdmmc_spi_write_block(lba, src);

    var code = priv_sdmmc_spi_cs_assert();
    if (code != core.err.ok) {
        logError("cs assert", code);
        return code;
    }
    // Pre-erase hint (ACMD23): best-effort -- ignore the response so a card
    // that does not implement it still streams correctly below.
    var acmd_r1: u8 = 0;
    _ = priv_sdmmc_spi_send_acmd(core.cmd.acmd_set_wr_blk_erase_count, count, @ptrCast(&acmd_r1));
    var r1: u8 = 0;
    code = priv_sdmmc_spi_send_command(core.cmd.write_multi_block, lbaArg(lba), @ptrCast(&r1));
    if (code != core.err.ok or r1 != 0) {
        _ = priv_sdmmc_spi_cs_release();
        return if (code != core.err.ok) code else core.err.protocol_error;
    }
    const result = writeMultiStream(src, count);
    _ = priv_sdmmc_spi_cs_release();
    return result;
}

// ---------------------------------------------------------------------------
// Erase and queries
// ---------------------------------------------------------------------------

pub export fn ra8_sdmmc_spi_erase_blocks(lba: u32, count: u32) callconv(.c) u16 {
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    if (count == 0) return core.err.invalid_arg;
    if (!core.multiBlockInRange(g_sdmmc_spi_state.capacity_blocks, lba, count)) {
        return core.err.out_of_range;
    }
    // Probe: erase only the FIRST block and read it back. The SD post-erase
    // value is card-dependent (0x00 on some, 0xFF on others), and the SCR
    // DATA_STAT_AFTER_ERASE bit's polarity is unreliable in practice, so
    // measure it. A non-zero read-back means this card erases to ones:
    // report "not supported" so the caller writes zeros -- and skip erasing
    // the rest, which would be wasted work.
    var code = eraseRange(lba, 1);
    if (code != core.err.ok) return code;

    var block: [core.block_size]u8 = @splat(0);
    code = ra8_sdmmc_spi_read_block(lba, &block);
    if (code != core.err.ok) return code;
    if (!core.erasedToZero(&block)) return core.err.not_supported;

    // The card erases to zero: erase the remaining range in one operation.
    if (count > 1) {
        code = eraseRange(lba + 1, count - 1);
        if (code != core.err.ok) return code;
    }
    return core.err.ok;
}

pub export fn ra8_sdmmc_spi_get_capacity(out_blocks: ?*u32) callconv(.c) u16 {
    const out = out_blocks orelse {
        ra8_log_emit_error(tag, "out_blocks is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    out.* = g_sdmmc_spi_state.capacity_blocks;
    return core.err.ok;
}

pub export fn ra8_sdmmc_spi_get_card_type(out_type: ?*u8) callconv(.c) u16 {
    const out = out_type orelse {
        ra8_log_emit_error(tag, "out_type is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    out.* = g_sdmmc_spi_state.card_type;
    return core.err.ok;
}

// ---------------------------------------------------------------------------
// ra8_fs backend adapter
// ---------------------------------------------------------------------------

fn fsReadBlock(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]u8) callconv(.c) u16 {
    _ = ctx;
    const dst = buf orelse return core.err.null_ptr;
    if (!core.fsLbaFits(lba)) return core.err.out_of_range;
    return ra8_sdmmc_spi_read_blocks(@truncate(lba), dst, count);
}

fn fsWriteBlock(ctx: ?*anyopaque, lba: u64, count: u32, buf: ?[*]const u8) callconv(.c) u16 {
    _ = ctx;
    const src = buf orelse return core.err.null_ptr;
    if (!core.fsLbaFits(lba)) return core.err.out_of_range;
    return ra8_sdmmc_spi_write_blocks(@truncate(lba), src, count);
}

fn fsEraseBlock(ctx: ?*anyopaque, lba: u64, count: u64) callconv(.c) u16 {
    _ = ctx;
    if (!core.fsLbaFits(lba) or !core.fsLbaFits(count)) return core.err.out_of_range;
    return ra8_sdmmc_spi_erase_blocks(@truncate(lba), @truncate(count));
}

fn fsGetCapacity(ctx: ?*anyopaque, block_count: ?*u64, block_size: ?*u32) callconv(.c) u16 {
    _ = ctx;
    const count_out = block_count orelse return core.err.null_ptr;
    const size_out = block_size orelse return core.err.null_ptr;
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    count_out.* = g_sdmmc_spi_state.capacity_blocks;
    size_out.* = core.block_size;
    return core.err.ok;
}

pub export fn ra8_sdmmc_spi_bind_fs_backend(out_backend: ?*FsBackend) callconv(.c) u16 {
    const out = out_backend orelse {
        ra8_log_emit_error(tag, "out_backend is null");
        return core.err.null_ptr;
    };
    if (!g_sdmmc_spi_state.initialized) return core.err.invalid_state;
    out.read_block = &fsReadBlock;
    out.write_block = &fsWriteBlock;
    out.get_capacity = &fsGetCapacity;
    out.erase_blocks = &fsEraseBlock;
    out.ctx = null;
    return core.err.ok;
}
