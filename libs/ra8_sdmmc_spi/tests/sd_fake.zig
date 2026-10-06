//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Every fake the `ra8_sdmmc_spi` tests run against, and every symbol the
//! archive links against: the scripted SD card and SPI transport, the fake HAL
//! standing in for the six `ra8_core_hal` symbols the SCI transport factory
//! calls, and the `ra8_log` sink. `rx_script` is consumed one byte per clocked
//! byte, and once it runs out the bus reads back idle, which is what a real
//! card does between responses. No test blocks live here: each test root
//! imports this module and drives it.

const std = @import("std");
const abi = @import("abi");

pub const err_ok: u16 = 0;
pub const err_invalid_arg: u16 = 0x103;
pub const err_hw_init_failed: u16 = 0x201;
pub const err_hw_timeout: u16 = 0x203;
pub const err_protocol_error: u16 = 0x406;
pub const err_null_ptr: u16 = 0x504;
pub const err_transport: u16 = 0x204;

pub const idle: u8 = 0xFF;

/// A fake SD card. `rx_script` is consumed one byte per clocked byte; once it
/// runs out the bus reads back idle, which is what a real card does between
/// responses.
pub const Card = struct {
    rx_script: []const u8 = &.{},
    rx_pos: usize = 0,
    tx_log: std.ArrayListUnmanaged(u8) = .{},
    cs_log: std.ArrayListUnmanaged(bool) = .{},
    clock_log: std.ArrayListUnmanaged(u32) = .{},
    xfer_calls: u32 = 0,
    bytes_clocked: u32 = 0,
    fail_xfer_after: ?u32 = null,
    fail_bulk_reads: bool = false,
    /// Refuse the one 512-byte bulk WRITE the payload path attempts, so the
    /// per-byte fallback the C carried is reachable.
    fail_bulk_512: bool = false,
    bulk_512_refusals: u32 = 0,
    fail_cs_on_call: ?u32 = null,
    cs_calls: u32 = 0,
    allocator: std.mem.Allocator,

    /// Responder mode: instead of a flat script, the card answers whatever
    /// command frame it is given, which is what makes the identification
    /// tests independent of how many idle bytes the driver clocks first.
    respond: bool = false,
    /// Big enough for a whole CMD18 answer: R1 plus two blocks of
    /// token + 512 payload bytes + CRC16.
    queue: [1200]u8 = undefined,
    queue_len: usize = 0,
    queue_pos: usize = 0,
    cmd0_r1: u8 = 0x01,
    cmd8_r1: u8 = 0x01,
    cmd8_echo: u32 = 0x0000_01AA,
    ocr: u32 = 0xC0FF_8000,
    cmd9_r1: u8 = 0x00,
    cmd16_r1: u8 = 0x00,
    csd: [16]u8 = blk: {
        var c: [16]u8 = @splat(0);
        c[0] = 0x40;
        c[8] = 0x1D;
        c[9] = 0xFF;
        break :blk c;
    },
    acmd41_ready_on: u32 = 2,
    acmd41_calls: u32 = 0,
    last_acmd41_arg: u32 = 0xFFFF_FFFF,
    answers_r1: bool = true,

    /// Data-phase knobs for the block I/O paths.
    data_fill: u8 = 0x5A,
    corrupt_read_crc: bool = false,
    stream_blocks: u32 = 2,
    write_response: u8 = 0x05,
    /// Write data-phase tracking: after a data-start token the card counts
    /// the 512 payload bytes plus the two CRC bytes, then answers with the
    /// data-response token followed by a not-busy byte.
    wr_active: bool = false,
    wr_count: u32 = 0,
    wr_blocks_seen: u32 = 0,

    pub fn init(allocator: std.mem.Allocator) Card {
        return .{ .allocator = allocator };
    }

    pub fn deinit(self: *Card) void {
        self.tx_log.deinit(self.allocator);
        self.cs_log.deinit(self.allocator);
        self.clock_log.deinit(self.allocator);
    }

    fn next(self: *Card) u8 {
        if (self.queue_pos < self.queue_len) {
            const byte = self.queue[self.queue_pos];
            self.queue_pos += 1;
            return byte;
        }
        if (self.rx_pos < self.rx_script.len) {
            const byte = self.rx_script[self.rx_pos];
            self.rx_pos += 1;
            return byte;
        }
        return idle;
    }

    fn enqueue(self: *Card, bytes: []const u8) void {
        self.queue_len = 0;
        self.queue_pos = 0;
        for (bytes) |b| {
            self.queue[self.queue_len] = b;
            self.queue_len += 1;
        }
    }

    fn beWord(word: u32) [4]u8 {
        return .{
            @truncate(word >> 24),
            @truncate(word >> 16),
            @truncate(word >> 8),
            @truncate(word),
        };
    }

    /// Answer one six-byte command frame the way a card in SPI mode does:
    /// the frame bytes themselves read back idle, then the response tokens
    /// follow on the next clocked bytes.
    fn onFrame(self: *Card, frame: []const u8) void {
        const command = frame[0];
        const arg: u32 = (@as(u32, frame[1]) << 24) | (@as(u32, frame[2]) << 16) |
            (@as(u32, frame[3]) << 8) | @as(u32, frame[4]);
        if (!self.answers_r1) {
            self.enqueue(&.{});
            return;
        }
        var buf: [32]u8 = undefined;
        var n: usize = 0;
        switch (command) {
            0x40 => { // CMD0 GO_IDLE_STATE
                buf[n] = self.cmd0_r1;
                n += 1;
            },
            0x48 => { // CMD8 SEND_IF_COND
                buf[n] = self.cmd8_r1;
                n += 1;
                if ((self.cmd8_r1 & 0x04) == 0) {
                    for (beWord(self.cmd8_echo)) |b| {
                        buf[n] = b;
                        n += 1;
                    }
                }
            },
            0x77 => { // CMD55 APP_CMD
                buf[n] = 0x01;
                n += 1;
            },
            0x69 => { // ACMD41 SD_SEND_OP_COND
                self.acmd41_calls += 1;
                self.last_acmd41_arg = arg;
                const ready = self.acmd41_ready_on != 0 and
                    self.acmd41_calls >= self.acmd41_ready_on;
                buf[n] = if (ready) 0x00 else 0x01;
                n += 1;
            },
            0x7A => { // CMD58 READ_OCR
                buf[n] = 0x00;
                n += 1;
                for (beWord(self.ocr)) |b| {
                    buf[n] = b;
                    n += 1;
                }
            },
            0x49 => { // CMD9 SEND_CSD
                buf[n] = self.cmd9_r1;
                n += 1;
                if (self.cmd9_r1 == 0) {
                    buf[n] = 0xFE;
                    n += 1;
                    for (self.csd) |b| {
                        buf[n] = b;
                        n += 1;
                    }
                    buf[n] = 0x12;
                    n += 1;
                    buf[n] = 0x34;
                    n += 1;
                }
            },
            0x50 => { // CMD16 SET_BLOCKLEN
                buf[n] = self.cmd16_r1;
                n += 1;
            },
            0x51 => { // CMD17 READ_SINGLE_BLOCK
                buf[n] = 0x00;
                n += 1;
                var wide: [1200]u8 = undefined;
                @memcpy(wide[0..n], buf[0..n]);
                const end = self.appendReadBlock(&wide, n);
                self.enqueue(wide[0..end]);
                return;
            },
            0x52 => { // CMD18 READ_MULTIPLE_BLOCK
                var wide: [1200]u8 = undefined;
                wide[0] = 0x00; // R1
                var end: usize = 1;
                var block: u32 = 0;
                while (block < self.stream_blocks) : (block += 1) {
                    end = self.appendReadBlock(&wide, end);
                }
                self.enqueue(wide[0..end]);
                return;
            },
            0x4C => { // CMD12 STOP_TRANSMISSION
                buf[n] = 0x7F; // stuff byte, then the real R1
                n += 1;
                buf[n] = 0x00;
                n += 1;
            },
            else => {
                buf[n] = 0x00;
                n += 1;
            },
        }
        self.enqueue(buf[0..n]);
    }

    /// Append one read data block (token, payload, CRC16) to `buf`.
    fn appendReadBlock(self: *Card, buf: []u8, at: usize) usize {
        var n = at;
        buf[n] = 0xFE;
        n += 1;
        const payload: [512]u8 = @splat(self.data_fill);
        @memcpy(buf[n .. n + 512], &payload);
        n += 512;
        var crc = abi.ra8_sdmmc_spi_crc16(&payload, 512);
        if (self.corrupt_read_crc) crc ^= 0xFFFF;
        buf[n] = @truncate(crc >> 8);
        n += 1;
        buf[n] = @truncate(crc);
        n += 1;
        return n;
    }

    /// Track the write data phase: a data-start token opens it, and once the
    /// 512 payload bytes and the two CRC bytes have gone by, the card queues
    /// its data-response token plus a not-busy byte.
    fn onWriteByte(self: *Card, byte: u8) void {
        if (!self.wr_active) {
            if (byte == 0xFE or byte == 0xFC) {
                self.wr_active = true;
                self.wr_count = 0;
            }
            return;
        }
        self.wr_count += 1;
        if (self.wr_count < 514) return;
        self.wr_active = false;
        self.wr_blocks_seen += 1;
        self.enqueue(&.{ self.write_response, idle });
    }
};

var g_card: ?*Card = null;

pub fn cardSetClock(_: ?*anyopaque, hz: u32) callconv(.c) u16 {
    const card = g_card.?;
    card.clock_log.append(card.allocator, hz) catch return err_transport;
    return err_ok;
}

pub fn cardCs(_: ?*anyopaque, asserted: bool) callconv(.c) u16 {
    const card = g_card.?;
    card.cs_calls += 1;
    if (card.fail_cs_on_call) |n| {
        if (card.cs_calls == n) return err_transport;
    }
    card.cs_log.append(card.allocator, asserted) catch return err_transport;
    return err_ok;
}

pub fn cardXfer(_: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u32) callconv(.c) u16 {
    const card = g_card.?;
    card.xfer_calls += 1;
    if (card.fail_bulk_reads and tx == null) return err_transport;
    if (card.fail_bulk_512 and tx != null and len == 512) {
        card.bulk_512_refusals += 1;
        return err_transport;
    }
    if (card.fail_xfer_after) |limit| {
        if (card.xfer_calls > limit) return err_transport;
    }
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        const out: u8 = if (tx) |t| t[i] else idle;
        card.tx_log.append(card.allocator, out) catch return err_transport;
        const in = card.next();
        if (rx) |r| r[i] = in;
        card.bytes_clocked += 1;
        card.onWriteByte(out);
    }
    if (card.respond and tx != null and len == 6) {
        card.onFrame(tx.?[0..6]);
    }
    return err_ok;
}

pub fn bind(card: *Card) void {
    g_card = card;
    abi.g_sdmmc_spi_state = .{};
    abi.g_sdmmc_spi_state.transport = .{
        .set_clock = cardSetClock,
        .cs = cardCs,
        .xfer = cardXfer,
        .ctx = null,
    };
}

pub fn bindResponder(card: *Card) void {
    card.respond = true;
    bind(card);
}

// The archive logs through ra8_log, so the test binary supplies the sink.
pub var last_tag: [*:0]const u8 = "";
pub var last_message: [*:0]const u8 = "";
pub var last_value: u32 = 0;
pub var log_lines: u32 = 0;

export fn ra8_log_emit_error(tag: ?[*:0]const u8, message: ?[*:0]const u8) void {
    if (tag) |t| last_tag = t;
    if (message) |m| last_message = m;
    log_lines += 1;
}

export fn ra8_log_emit_error_val(tag: ?[*:0]const u8, _: ?[*:0]const u8, value: u32) void {
    if (tag) |t| last_tag = t;
    last_value = value;
    log_lines += 1;
}

/// The HAL the SCI transport factory drives. The archive references these
/// six symbols and the production link resolves them out of `ra8_core_hal`;
/// here the test binary supplies them so the factory's routing order, its
/// first-failure behaviour and all three shims are actually observable.
pub const Hal = struct {
    routed: [8][2]u32 = undefined,
    routed_len: usize = 0,
    route_fail_on: ?usize = null,
    output_init: ?[2]u32 = null,
    output_init_fail: bool = false,
    sci_init_channel: ?u8 = null,
    sci_init_cfg: ?abi.SciSpiCfg = null,
    sci_init_fail: bool = false,
    set_clock_calls: [8][3]u32 = undefined,
    set_clock_len: usize = 0,
    gpio_writes: [8][2]u32 = undefined,
    gpio_writes_len: usize = 0,
    xfer_channel: ?u8 = null,
    xfer_len: u32 = 0,
};

pub var g_hal: Hal = .{};

pub const err_hal: u16 = 0x205;

export fn ra8_pfs_route_peripheral(pin: u16, sel: u8, owner: ?[*:0]const u8) u16 {
    _ = owner;
    if (g_hal.route_fail_on) |n| {
        if (g_hal.routed_len == n) return err_hal;
    }
    g_hal.routed[g_hal.routed_len] = .{ pin, sel };
    g_hal.routed_len += 1;
    return err_ok;
}

/// The pin vtable the factory resolves. Its rows record exactly what the
/// GPIO fakes used to, so the driver suite still asserts on `output_init`
/// and `gpio_writes`.
const PinInterface = extern struct {
    output_init: *const fn (ctx: ?*anyopaque, pin: u16, init_level: u8) callconv(.c) u16,
    input_init: *const fn (ctx: ?*anyopaque, pin: u16, pull: u8) callconv(.c) u16,
    write: *const fn (ctx: ?*anyopaque, pin: u16, lvl: u8) callconv(.c) u16,
    read: *const fn (ctx: ?*anyopaque, pin: u16, out_lvl: *u8) callconv(.c) u16,
    toggle: *const fn (ctx: ?*anyopaque, pin: u16) callconv(.c) u16,
    release: *const fn (ctx: ?*anyopaque, pin: u16) callconv(.c) u16,
    ctx: ?*anyopaque,
};

fn pinOutputInit(ctx: ?*anyopaque, pin: u16, init_level: u8) callconv(.c) u16 {
    _ = ctx;
    if (g_hal.output_init_fail) return err_hal;
    g_hal.output_init = .{ pin, init_level };
    return err_ok;
}

fn pinWrite(ctx: ?*anyopaque, pin: u16, lvl: u8) callconv(.c) u16 {
    _ = ctx;
    g_hal.gpio_writes[g_hal.gpio_writes_len] = .{ pin, lvl };
    g_hal.gpio_writes_len += 1;
    return err_ok;
}

fn pinInputInit(ctx: ?*anyopaque, pin: u16, pull: u8) callconv(.c) u16 {
    _ = .{ ctx, pin, pull };
    return err_ok;
}

fn pinRead(ctx: ?*anyopaque, pin: u16, out_lvl: *u8) callconv(.c) u16 {
    _ = .{ ctx, pin };
    out_lvl.* = 0;
    return err_ok;
}

fn pinToggle(ctx: ?*anyopaque, pin: u16) callconv(.c) u16 {
    _ = .{ ctx, pin };
    return err_ok;
}

fn pinRelease(ctx: ?*anyopaque, pin: u16) callconv(.c) u16 {
    _ = .{ ctx, pin };
    return err_ok;
}

const g_pin_interface: PinInterface = .{
    .output_init = pinOutputInit,
    .input_init = pinInputInit,
    .write = pinWrite,
    .read = pinRead,
    .toggle = pinToggle,
    .release = pinRelease,
    .ctx = null,
};

export fn ra8_pin_interface_default() *const PinInterface {
    return &g_pin_interface;
}

export fn ra8_sci_spi_init(channel: u8, cfg: ?*const abi.SciSpiCfg) u16 {
    if (g_hal.sci_init_fail) return err_hal;
    g_hal.sci_init_channel = channel;
    if (cfg) |c| g_hal.sci_init_cfg = c.*;
    return err_ok;
}

export fn ra8_sci_spi_set_clock(channel: u8, baud_hz: u32, pclk_hz: u32) u16 {
    g_hal.set_clock_calls[g_hal.set_clock_len] = .{ channel, baud_hz, pclk_hz };
    g_hal.set_clock_len += 1;
    return err_ok;
}

export fn ra8_sci_spi_xfer(channel: u8, tx: ?[*]const u8, rx: ?[*]u8, len: u32) u16 {
    _ = tx;
    _ = rx;
    g_hal.xfer_channel = channel;
    g_hal.xfer_len = len;
    return err_ok;
}
