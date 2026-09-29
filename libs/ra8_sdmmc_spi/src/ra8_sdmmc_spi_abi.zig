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
