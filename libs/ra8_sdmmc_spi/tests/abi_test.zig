//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `ra8_sdmmc_spi` core ABI membrane, driven over a
//! scripted fake transport that records every byte clocked. The fake answers
//! the SD identification sequence the way a real card does, so the tests pin
//! the command order, the CS discipline around each command, the bounded
//! polls and the published card type and capacity.

const std = @import("std");
const abi = @import("abi");
const testing = std.testing;

const err_ok: u16 = 0;
const err_invalid_arg: u16 = 0x103;
const err_hw_init_failed: u16 = 0x201;
const err_hw_timeout: u16 = 0x203;
const err_protocol_error: u16 = 0x406;
const err_null_ptr: u16 = 0x504;
const err_transport: u16 = 0x204;

const idle: u8 = 0xFF;

/// A fake SD card. `rx_script` is consumed one byte per clocked byte; once it
/// runs out the bus reads back idle, which is what a real card does between
/// responses.
const Card = struct {
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
        var c = [_]u8{0} ** 16;
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

    fn init(allocator: std.mem.Allocator) Card {
        return .{ .allocator = allocator };
    }

    fn deinit(self: *Card) void {
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

fn cardSetClock(_: ?*anyopaque, hz: u32) callconv(.c) u16 {
    const card = g_card.?;
    card.clock_log.append(card.allocator, hz) catch return err_transport;
    return err_ok;
}

fn cardCs(_: ?*anyopaque, asserted: bool) callconv(.c) u16 {
    const card = g_card.?;
    card.cs_calls += 1;
    if (card.fail_cs_on_call) |n| {
        if (card.cs_calls == n) return err_transport;
    }
    card.cs_log.append(card.allocator, asserted) catch return err_transport;
    return err_ok;
}

fn cardXfer(_: ?*anyopaque, tx: ?[*]const u8, rx: ?[*]u8, len: u32) callconv(.c) u16 {
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

fn bind(card: *Card) void {
    g_card = card;
    abi.g_sdmmc_spi_state = .{};
    abi.g_sdmmc_spi_state.transport = .{
        .set_clock = cardSetClock,
        .cs = cardCs,
        .xfer = cardXfer,
        .ctx = null,
    };
}

test "crc7 and crc16 are reachable through the C ABI" {
    const frame = [_]u8{ 0x40, 0, 0, 0, 0 };
    try testing.expectEqual(@as(u8, 0x4A), abi.ra8_sdmmc_spi_crc7(&frame, 5));
    const data = "123456789";
    try testing.expectEqual(@as(u16, 0x31C3), abi.ra8_sdmmc_spi_crc16(data.ptr, data.len));
    try testing.expectEqual(@as(u8, 0), abi.ra8_sdmmc_spi_crc7(null, 5));
    try testing.expectEqual(@as(u16, 0), abi.ra8_sdmmc_spi_crc16(null, 5));
}

test "xfer_one clocks one byte and publishes the response" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{0x5A};
    bind(&card);

    var rx: u8 = 0;
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_xfer_one(0xA5, @ptrCast(&rx)));
    try testing.expectEqual(@as(u8, 0x5A), rx);
    try testing.expectEqual(@as(usize, 1), card.tx_log.items.len);
    try testing.expectEqual(@as(u8, 0xA5), card.tx_log.items[0]);
}

test "xfer_one accepts a null receive pointer and discards the byte" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_xfer_one(0x00, null));
    try testing.expectEqual(@as(u32, 1), card.bytes_clocked);
}

test "xfer_one propagates the transport's own error unchanged" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.fail_xfer_after = 0;
    bind(&card);
    var rx: u8 = 0xEE;
    try testing.expectEqual(err_transport, abi.priv_sdmmc_spi_xfer_one(0x01, @ptrCast(&rx)));
    // A failed exchange must not publish a byte.
    try testing.expectEqual(@as(u8, 0xEE), rx);
}

test "send_idle clocks exactly n idle bytes" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_idle(10));
    try testing.expectEqual(@as(u32, 10), card.bytes_clocked);
    for (card.tx_log.items) |byte| try testing.expectEqual(idle, byte);
}

test "send_idle with a zero count touches the bus not at all" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_idle(0));
    try testing.expectEqual(@as(u32, 0), card.xfer_calls);
}

test "cs_assert drives CS low then clocks one byte so CIPO is sampled" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_cs_assert());
    try testing.expectEqual(@as(usize, 1), card.cs_log.items.len);
    try testing.expect(card.cs_log.items[0]);
    try testing.expectEqual(@as(u32, 1), card.bytes_clocked);
}

test "cs_release drives CS high then clocks one trailing byte" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_cs_release());
    try testing.expectEqual(@as(usize, 1), card.cs_log.items.len);
    try testing.expect(!card.cs_log.items[0]);
    try testing.expectEqual(@as(u32, 1), card.bytes_clocked);
}

test "a failing cs callback aborts before any byte is clocked" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.fail_cs_on_call = 1;
    bind(&card);
    try testing.expectEqual(err_transport, abi.priv_sdmmc_spi_cs_assert());
    try testing.expectEqual(@as(u32, 0), card.bytes_clocked);
}

test "send_command writes the six-byte frame then captures R1" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    // Six bytes of frame echo, then one non-R1 byte, then the R1.
    card.rx_script = &[_]u8{ idle, idle, idle, idle, idle, idle, 0xFF, 0x01 };
    bind(&card);

    var r1: u8 = 0;
    try testing.expectEqual(
        err_ok,
        abi.priv_sdmmc_spi_send_command(0x40, 0, @ptrCast(&r1)),
    );
    try testing.expectEqual(@as(u8, 0x01), r1);
    try testing.expectEqual(@as(u8, 0x40), card.tx_log.items[0]);
    try testing.expectEqual(@as(u8, 0x95), card.tx_log.items[5]);
}

test "send_command times out after the bounded R1 window" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bind(&card); // the card never drops the sentinel bit
    var r1: u8 = 0;
    try testing.expectEqual(
        err_hw_timeout,
        abi.priv_sdmmc_spi_send_command(0x51, 0, @ptrCast(&r1)),
    );
    // Six frame bytes plus exactly the R1 budget.
    try testing.expectEqual(@as(u32, 6 + 16), card.bytes_clocked);
}

test "send_command accepts a null R1 destination" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{ idle, idle, idle, idle, idle, idle, 0x00 };
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_command(0x50, 0, null));
}

test "send_acmd prefixes CMD55 and returns the application command's R1" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{
        idle, idle, idle, idle, idle, idle, 0x01, // CMD55 R1
        idle, idle, idle, idle, idle, idle, 0x00, // ACMD41 R1
    };
    bind(&card);

    var r1: u8 = 0xFF;
    try testing.expectEqual(
        err_ok,
        abi.priv_sdmmc_spi_send_acmd(0x69, 0x4000_0000, @ptrCast(&r1)),
    );
    try testing.expectEqual(@as(u8, 0x00), r1);
    try testing.expectEqual(@as(u8, 0x77), card.tx_log.items[0]);
    try testing.expectEqual(@as(u8, 0x69), card.tx_log.items[7]);
}

test "send_stop_transmission discards the stuff byte before polling R1" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    // Frame, then one stuff byte that would read as a valid R1, then the real R1.
    card.rx_script = &[_]u8{ idle, idle, idle, idle, idle, idle, 0x7F, 0x00 };
    bind(&card);

    var r1: u8 = 0xFF;
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_stop_transmission(@ptrCast(&r1)));
    try testing.expectEqual(@as(u8, 0x00), r1);
    try testing.expectEqual(@as(u8, 0x4C), card.tx_log.items[0]);
}

test "wait_data_token stops on the single-block start token" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{ idle, idle, 0xFE };
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_wait_data_token());
    try testing.expectEqual(@as(u32, 3), card.bytes_clocked);
}

test "wait_data_token does not accept the multi-block start token" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{0xFC};
    card.fail_xfer_after = 4;
    bind(&card);
    try testing.expectEqual(err_transport, abi.priv_sdmmc_spi_wait_data_token());
}

test "wait_not_busy_bounded honours its own budget" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{ 0x00, 0x00, 0x00 };
    bind(&card);
    try testing.expectEqual(err_hw_timeout, abi.priv_sdmmc_spi_wait_not_busy_bounded(2));
    try testing.expectEqual(@as(u32, 2), card.bytes_clocked);
}

test "wait_not_busy returns as soon as the card releases the bus" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.rx_script = &[_]u8{ 0x00, 0x00, idle };
    bind(&card);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_wait_not_busy());
    try testing.expectEqual(@as(u32, 3), card.bytes_clocked);
}

test "validate_transport keeps the C's null_ptr before invalid_arg order" {
    try testing.expectEqual(err_null_ptr, abi.priv_sdmmc_spi_validate_transport(null));

    var t = abi.Transport{
        .set_clock = cardSetClock,
        .cs = cardCs,
        .xfer = cardXfer,
        .ctx = null,
    };
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_validate_transport(&t));
    t.xfer = null;
    try testing.expectEqual(err_invalid_arg, abi.priv_sdmmc_spi_validate_transport(&t));
}

fn bindResponder(card: *Card) void {
    card.respond = true;
    bind(card);
}

test "run_init_sequence identifies an SDHC card and publishes its capacity" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(
        @as(u8, @intFromEnum(abi.CardType.sdhc)),
        abi.g_sdmmc_spi_state.card_type,
    );
    try testing.expectEqual(@as(u32, 7680 * 1024), abi.g_sdmmc_spi_state.capacity_blocks);
    // The sequence never publishes initialized itself; the I/O half does.
    try testing.expect(!abi.g_sdmmc_spi_state.initialized);
    // ACMD41 for a v2 card carries the HCS bit.
    try testing.expectEqual(@as(u32, 0x4000_0000), card.last_acmd41_arg);
}

test "run_init_sequence wakes the card, opens with CMD0 and ends with CS released" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    // The wake sequence is a CS release followed by ten idle bytes, so the
    // first command byte on the bus is CMD0 at index 11.
    try testing.expect(!card.cs_log.items[0]);
    try testing.expectEqual(@as(u8, 0x40), card.tx_log.items[11]);
    try testing.expect(!card.cs_log.items[card.cs_log.items.len - 1]);
}

test "a card whose CMD0 answer is not idle-state is retried then refused" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd0_r1 = 0x05;
    bindResponder(&card);

    try testing.expectEqual(err_protocol_error, abi.priv_sdmmc_spi_run_init_sequence());
    // Four recovery attempts, each moving CS more than once.
    try testing.expect(card.cs_calls > 8);
}

test "a card that never answers R1 at all times out" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.answers_r1 = false;
    bindResponder(&card);
    try testing.expectEqual(err_hw_timeout, abi.priv_sdmmc_spi_run_init_sequence());
}

test "run_init_sequence answers protocol_error on a mismatched CMD8 echo" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd8_echo = 0x0000_01AB;
    bindResponder(&card);
    try testing.expectEqual(err_protocol_error, abi.priv_sdmmc_spi_run_init_sequence());
}

test "a v1 card that rejects CMD8 skips the OCR read and drops the HCS bit" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd8_r1 = 0x05; // illegal command: this is a v1.x card
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(@as(u32, 0), card.last_acmd41_arg);
    try testing.expectEqual(
        @as(u8, @intFromEnum(abi.CardType.sdv1)),
        abi.g_sdmmc_spi_state.card_type,
    );
}

test "a v2 card with CCS clear classifies as standard capacity" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.ocr = 0x8000_0000; // busy bit only, no CCS
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(
        @as(u8, @intFromEnum(abi.CardType.sdv2)),
        abi.g_sdmmc_spi_state.card_type,
    );
}

test "ACMD41 is retried until the idle bit clears" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.acmd41_ready_on = 5;
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(@as(u32, 5), card.acmd41_calls);
}

test "a card that stays busy through the ACMD41 budget fails init" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.acmd41_ready_on = 0; // never ready
    bindResponder(&card);

    try testing.expectEqual(err_hw_init_failed, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(@as(u32, 1000), card.acmd41_calls);
}

test "run_init_sequence rejects a CSD that decodes to zero blocks" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.csd = [_]u8{0} ** 16;
    bindResponder(&card);

    try testing.expectEqual(err_protocol_error, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(@as(u32, 0), abi.g_sdmmc_spi_state.capacity_blocks);
}

test "a non-zero CMD9 response byte is a protocol error, not a capacity" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd9_r1 = 0x04;
    bindResponder(&card);
    try testing.expectEqual(err_protocol_error, abi.priv_sdmmc_spi_run_init_sequence());
}

test "a non-zero CMD16 response byte refuses the card after CSD decoding" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd16_r1 = 0x40;
    bindResponder(&card);
    try testing.expectEqual(err_protocol_error, abi.priv_sdmmc_spi_run_init_sequence());
    // The capacity is only published once SET_BLOCKLEN succeeds.
    try testing.expectEqual(@as(u32, 0), abi.g_sdmmc_spi_state.capacity_blocks);
}

test "a CSD v1 card publishes the mult/blocklen capacity" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.cmd8_r1 = 0x05;
    var csd = [_]u8{0} ** 16;
    csd[5] = 0x09;
    csd[6] = 0x03;
    csd[7] = 0xA5;
    csd[8] = 0xC0;
    csd[9] = 0x03;
    csd[10] = 0x80;
    card.csd = csd;
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expect(abi.g_sdmmc_spi_state.capacity_blocks > 0);
}

test "the R3/R7 tail falls back to byte-at-a-time when the bulk read fails" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.fail_bulk_reads = true; // every tx == null transfer is refused
    bindResponder(&card);

    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_run_init_sequence());
    try testing.expectEqual(
        @as(u8, @intFromEnum(abi.CardType.sdhc)),
        abi.g_sdmmc_spi_state.card_type,
    );
}

test "the state object survives a rebind with a fresh transport" {
    var first = Card.init(testing.allocator);
    defer first.deinit();
    bind(&first);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_idle(3));
    try testing.expectEqual(@as(u32, 3), first.bytes_clocked);

    var second = Card.init(testing.allocator);
    defer second.deinit();
    bind(&second);
    try testing.expectEqual(err_ok, abi.priv_sdmmc_spi_send_idle(2));
    try testing.expectEqual(@as(u32, 2), second.bytes_clocked);
    try testing.expectEqual(@as(u32, 3), first.bytes_clocked);
}

// The archive logs through ra8_log, so the test binary supplies the sink.
var last_tag: [*:0]const u8 = "";
var last_message: [*:0]const u8 = "";
var last_value: u32 = 0;
var log_lines: u32 = 0;

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

test "only CMD9 and CMD16 failures log, and they log under the SDSPI tag" {
    log_lines = 0;

    // A probe that fails before CMD9 logs nothing at all.
    var first = Card.init(testing.allocator);
    defer first.deinit();
    first.answers_r1 = false;
    first.respond = true;
    bind(&first);
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 0), log_lines);

    // A CSD that decodes to zero blocks logs the message then the code.
    var second = Card.init(testing.allocator);
    defer second.deinit();
    second.csd = [_]u8{0} ** 16;
    second.respond = true;
    bind(&second);
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 2), log_lines);
    try testing.expectEqualStrings("SDSPI", std.mem.span(last_tag));
    try testing.expectEqual(@as(u32, err_protocol_error), last_value);

    // A SET_BLOCKLEN refusal logs its own message under the same tag.
    var third = Card.init(testing.allocator);
    defer third.deinit();
    third.cmd16_r1 = 0x40;
    third.respond = true;
    bind(&third);
    log_lines = 0;
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 2), log_lines);
    try testing.expectEqualStrings("SDSPI", std.mem.span(last_tag));
}

// ---------------------------------------------------------------------------
// Block I/O and the SCI transport factory
// ---------------------------------------------------------------------------

const err_invalid_state: u16 = 0x104;
const err_not_supported: u16 = 0x107;
const err_out_of_range: u16 = 0x208;
const err_crc_mismatch: u16 = 0x405;

/// The HAL the SCI transport factory drives. The archive references these
/// six symbols and the production link resolves them out of `ra8_core_hal`;
/// here the test binary supplies them so the factory's routing order, its
/// first-failure behaviour and all three shims are actually observable.
const Hal = struct {
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

var g_hal: Hal = .{};

const err_hal: u16 = 0x205;

export fn ra8_pfs_route_peripheral(pin: u16, sel: u8, owner: ?[*:0]const u8) u16 {
    _ = owner;
    if (g_hal.route_fail_on) |n| {
        if (g_hal.routed_len == n) return err_hal;
    }
    g_hal.routed[g_hal.routed_len] = .{ pin, sel };
    g_hal.routed_len += 1;
    return err_ok;
}

export fn ra8_gpio_output_init(pin: u16, init_level: u8) u16 {
    if (g_hal.output_init_fail) return err_hal;
    g_hal.output_init = .{ pin, init_level };
    return err_ok;
}

export fn ra8_gpio_write(pin: u16, lvl: u8) u16 {
    g_hal.gpio_writes[g_hal.gpio_writes_len] = .{ pin, lvl };
    g_hal.gpio_writes_len += 1;
    return err_ok;
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

const pins: abi.SciPins = .{ .sck = 0x0102, .cipo = 0x0103, .copi = 0x0104, .cs = 0x0105 };

/// Bring the driver up as initialised without running the identification
/// sequence: the read/write/erase paths only read `capacity_blocks` and
/// `card_type`, so setting them directly keeps each test to one subject.
fn openWith(card: *Card, card_type: abi.CardType, capacity: u32) void {
    bind(card);
    abi.g_sdmmc_spi_state.card_type = @intFromEnum(card_type);
    abi.g_sdmmc_spi_state.capacity_blocks = capacity;
    abi.g_sdmmc_spi_state.initialized = true;
}

test "the factory routes all three signal pins then claims CS high" {
    g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_transport_sci(3, 60_000_000, &pins, &transport));

    try testing.expectEqual(@as(usize, 3), g_hal.routed_len);
    try testing.expectEqual(@as(u32, pins.sck), g_hal.routed[0][0]);
    try testing.expectEqual(@as(u32, pins.cipo), g_hal.routed[1][0]);
    try testing.expectEqual(@as(u32, pins.copi), g_hal.routed[2][0]);
    for (g_hal.routed[0..3]) |entry| try testing.expectEqual(@as(u32, 0x04), entry[1]);

    // CS is a plain GPIO output, parked high (deselected).
    try testing.expectEqual([2]u32{ pins.cs, 1 }, g_hal.output_init.?);

    // The channel comes up at the mandatory init clock, mode 0, MSB first.
    try testing.expectEqual(@as(u8, 3), g_hal.sci_init_channel.?);
    const cfg = g_hal.sci_init_cfg.?;
    try testing.expectEqual(@as(u32, 400_000), cfg.baud_hz);
    try testing.expectEqual(@as(u32, 60_000_000), cfg.pclk_hz);
    try testing.expectEqual(@as(u8, 0), cfg.mode);
    try testing.expect(!cfg.lsb_first);

    try testing.expect(transport.set_clock != null);
    try testing.expect(transport.cs != null);
    try testing.expect(transport.xfer != null);
    try testing.expect(transport.ctx != null);
}

test "the factory guards its pointers then the PCLKA rate" {
    g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(
        err_null_ptr,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, null, &transport),
    );
    try testing.expectEqual(
        err_null_ptr,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, null),
    );
    try testing.expectEqual(
        err_invalid_arg,
        abi.ra8_sdmmc_spi_transport_sci(0, 0, &pins, &transport),
    );
    // A refused argument never touches the HAL.
    try testing.expectEqual(@as(usize, 0), g_hal.routed_len);
}

test "the factory stops at the first pin that will not route" {
    g_hal = .{};
    g_hal.route_fail_on = 1; // CIPO refuses.
    var transport: abi.Transport = .{};
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expectEqual(@as(usize, 1), g_hal.routed_len);
    try testing.expect(g_hal.output_init == null);
    try testing.expect(g_hal.sci_init_channel == null);
    // The out descriptor is left untouched on the failure path.
    try testing.expect(transport.xfer == null);
}

test "the factory propagates a CS claim failure and an SCI bring-up failure" {
    var transport: abi.Transport = .{};
    g_hal = .{};
    g_hal.output_init_fail = true;
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expect(g_hal.sci_init_channel == null);

    g_hal = .{};
    g_hal.sci_init_fail = true;
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expectEqual(@as(usize, 3), g_hal.routed_len);
}

test "the factory's shims reach the SCI channel and the CS pin" {
    g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_transport_sci(7, 48_000_000, &pins, &transport));

    try testing.expectEqual(err_ok, transport.set_clock.?(transport.ctx, 25_000_000));
    try testing.expectEqual([3]u32{ 7, 25_000_000, 48_000_000 }, g_hal.set_clock_calls[0]);

    // Active-low select: asserted drives the pin low.
    try testing.expectEqual(err_ok, transport.cs.?(transport.ctx, true));
    try testing.expectEqual(err_ok, transport.cs.?(transport.ctx, false));
    try testing.expectEqual([2]u32{ pins.cs, 0 }, g_hal.gpio_writes[0]);
    try testing.expectEqual([2]u32{ pins.cs, 1 }, g_hal.gpio_writes[1]);

    var byte: u8 = 0xAA;
    try testing.expectEqual(err_ok, transport.xfer.?(transport.ctx, @ptrCast(&byte), null, 1));
    try testing.expectEqual(@as(u8, 7), g_hal.xfer_channel.?);
    try testing.expectEqual(@as(u32, 1), g_hal.xfer_len);
}

test "every factory shim refuses a null bus context" {
    g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport));
    try testing.expectEqual(err_null_ptr, transport.set_clock.?(null, 400_000));
    try testing.expectEqual(err_null_ptr, transport.cs.?(null, true));
    try testing.expectEqual(err_null_ptr, transport.xfer.?(null, null, null, 1));
}

test "init runs the identification sequence between the two clock changes" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    bind(&card);
    const transport = abi.g_sdmmc_spi_state.transport;

    abi.g_sdmmc_spi_state = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_init(&transport));
    try testing.expect(abi.g_sdmmc_spi_state.initialized);

    // First the 400 kHz floor, last the 25 MHz data clock.
    try testing.expect(card.clock_log.items.len >= 2);
    try testing.expectEqual(@as(u32, 400_000), card.clock_log.items[0]);
    try testing.expectEqual(
        @as(u32, 25_000_000),
        card.clock_log.items[card.clock_log.items.len - 1],
    );
}

test "init refuses a second open and a transport with a missing callback" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    bind(&card);
    const transport = abi.g_sdmmc_spi_state.transport;

    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_init(null));
    var holed = transport;
    holed.xfer = null;
    try testing.expectEqual(err_invalid_arg, abi.ra8_sdmmc_spi_init(&holed));

    abi.g_sdmmc_spi_state = .{};
    abi.g_sdmmc_spi_state.transport = transport;
    abi.g_sdmmc_spi_state.initialized = true;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_init(&transport));
}

test "deinit closes the handle and clears the probe results" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 1024);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_deinit());
    try testing.expect(!abi.g_sdmmc_spi_state.initialized);
    try testing.expectEqual(@as(u32, 0), abi.g_sdmmc_spi_state.capacity_blocks);
    try testing.expectEqual(
        @intFromEnum(abi.CardType.unknown),
        abi.g_sdmmc_spi_state.card_type,
    );
}

test "the queries publish capacity and card type only while open" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 4096);

    var blocks: u32 = 0;
    var kind: u8 = 0xFF;
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_get_capacity(&blocks));
    try testing.expectEqual(@as(u32, 4096), blocks);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_get_card_type(&kind));
    try testing.expectEqual(@intFromEnum(abi.CardType.sdhc), kind);

    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_get_capacity(null));
    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_get_card_type(null));

    abi.g_sdmmc_spi_state.initialized = false;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_get_capacity(&blocks));
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_get_card_type(&kind));
}

test "read_block clocks a block out and checks its CRC" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.data_fill = 0x5A;
    openWith(&card, .sdhc, 8);

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_block(3, &block));
    for (block) |byte| try testing.expectEqual(@as(u8, 0x5A), byte);
    // CS was asserted and released exactly once.
    try testing.expectEqual(@as(usize, 2), card.cs_log.items.len);
    try testing.expect(card.cs_log.items[0]);
    try testing.expect(!card.cs_log.items[1]);
}

test "read_block reports a CRC mismatch when the trailer disagrees" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.corrupt_read_crc = true;
    openWith(&card, .sdhc, 8);

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_crc_mismatch, abi.ra8_sdmmc_spi_read_block(0, &block));
    // CS is still released on the error path.
    try testing.expect(!card.cs_log.items[card.cs_log.items.len - 1]);
}

test "read_block guards the buffer, the handle and the card bounds" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    var block: [512]u8 = @splat(0);
    openWith(&card, .sdhc, 8);

    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_read_block(0, null));
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_read_block(8, &block));
    abi.g_sdmmc_spi_state.initialized = false;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_read_block(0, &block));
}

/// Find the argument of the first `command` frame in the transmit log.
fn frameArg(log: []const u8, command: u8) ?u32 {
    if (log.len < 6) return null;
    var i: usize = 0;
    while (i + 5 < log.len) : (i += 1) {
        if (log[i] != command) continue;
        return (@as(u32, log[i + 1]) << 24) | (@as(u32, log[i + 2]) << 16) |
            (@as(u32, log[i + 3]) << 8) | @as(u32, log[i + 4]);
    }
    return null;
}

test "read_block addresses a byte-addressed card in bytes" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdv1, 8);

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_block(2, &block));
    // The CMD17 frame carries the byte offset 2 * 512 == 0x400.
    try testing.expectEqual(@as(u32, 0x400), frameArg(card.tx_log.items, 0x51).?);
}

test "read_block addresses a block-addressed card in blocks" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdhc, 8);

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_block(2, &block));
    try testing.expectEqual(@as(u32, 2), frameArg(card.tx_log.items, 0x51).?);
}

test "read_blocks of zero blocks is a no-op and of one block takes CMD17" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdhc, 8);

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_blocks(0, &block, 0));
    try testing.expectEqual(@as(u32, 0), card.xfer_calls);

    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_blocks(1, &block, 1));
    // CMD18 must never appear on the single-block path.
    try testing.expect(frameArg(card.tx_log.items, 0x52) == null);
    try testing.expect(frameArg(card.tx_log.items, 0x51) != null);
}

test "read_blocks refuses a run that leaves the card" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 8);
    var buf: [1024]u8 = @splat(0);
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_read_blocks(7, &buf, 2));
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_read_blocks(8, &buf, 1));
    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_read_blocks(0, null, 2));
}

test "read_blocks streams two blocks under one CMD18 and stops the stream" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.data_fill = 0x41;
    card.stream_blocks = 2;
    openWith(&card, .sdhc, 8);

    var buf: [1024]u8 = @splat(0);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_read_blocks(0, &buf, 2));
    for (buf) |byte| try testing.expectEqual(@as(u8, 0x41), byte);

    // One CMD18 for the run, and CMD12 to leave the data state.
    try testing.expect(frameArg(card.tx_log.items, 0x52) != null);
    try testing.expect(frameArg(card.tx_log.items, 0x4C) != null);
    // CS was held across the whole stream: one assert, one release.
    try testing.expectEqual(@as(usize, 2), card.cs_log.items.len);
}

test "write_block sends CMD24, the payload and its CRC, then waits out the program" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdhc, 8);

    const payload: [512]u8 = @splat(0x7E);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_write_block(1, &payload));
    try testing.expectEqual(@as(u32, 1), card.wr_blocks_seen);
    try testing.expectEqual(@as(u32, 1), frameArg(card.tx_log.items, 0x58).?);

    // The single-block start token is followed by the payload and the true
    // CRC16 over it, high byte first.
    const crc = abi.ra8_sdmmc_spi_crc16(&payload, 512);
    const log = card.tx_log.items;
    var saw_token = false;
    for (0..log.len) |i| {
        if (log[i] == 0xFE and i + 514 < log.len) {
            try testing.expectEqual(@as(u8, @truncate(crc >> 8)), log[i + 513]);
            try testing.expectEqual(@as(u8, @truncate(crc)), log[i + 514]);
            saw_token = true;
            break;
        }
    }
    try testing.expect(saw_token);
}

test "write_block treats a rejected data response as a protocol error" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.write_response = 0x0B; // CRC-error verdict
    openWith(&card, .sdhc, 8);

    const payload: [512]u8 = @splat(0);
    try testing.expectEqual(err_protocol_error, abi.ra8_sdmmc_spi_write_block(0, &payload));
    try testing.expect(!card.cs_log.items[card.cs_log.items.len - 1]);
}

test "write_block guards the buffer, the handle and the card bounds" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    const payload: [512]u8 = @splat(0);
    openWith(&card, .sdhc, 8);
    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_write_block(0, null));
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_write_block(8, &payload));
    abi.g_sdmmc_spi_state.initialized = false;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_write_block(0, &payload));
}

test "write_blocks of zero is a no-op, of one takes CMD24, of many takes CMD25" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdhc, 8);

    const payload: [1024]u8 = @splat(0x10);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_write_blocks(0, &payload, 0));
    try testing.expectEqual(@as(u32, 0), card.xfer_calls);
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_write_blocks(7, &payload, 2));
    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_write_blocks(0, null, 2));

    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_write_blocks(0, &payload, 1));
    try testing.expect(frameArg(card.tx_log.items, 0x58) != null);
    try testing.expect(frameArg(card.tx_log.items, 0x59) == null);
}

test "write_blocks hints ACMD23 then streams both blocks under one CMD25" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    openWith(&card, .sdhc, 8);

    const payload: [1024]u8 = @splat(0x21);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_write_blocks(0, &payload, 2));
    try testing.expectEqual(@as(u32, 2), card.wr_blocks_seen);

    // ACMD23 carries the block count as its hint, then one CMD25 for the run.
    try testing.expectEqual(@as(u32, 2), frameArg(card.tx_log.items, 0x57).?);
    try testing.expect(frameArg(card.tx_log.items, 0x59) != null);

    var multi_tokens: u32 = 0;
    var saw_stop_tran = false;
    for (card.tx_log.items) |byte| {
        if (byte == 0xFC) multi_tokens += 1;
        if (byte == 0xFD) saw_stop_tran = true;
    }
    try testing.expectEqual(@as(u32, 2), multi_tokens);
    try testing.expect(saw_stop_tran);
}

test "write falls back to per-byte exchange when a bulk write is refused" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.fail_bulk_512 = true;
    openWith(&card, .sdhc, 8);

    const payload: [512]u8 = @splat(0x44);
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_write_block(0, &payload));
    try testing.expectEqual(@as(u32, 1), card.bulk_512_refusals);
    // Every payload byte still reached the bus, one exchange at a time.
    var matches: u32 = 0;
    for (card.tx_log.items) |byte| {
        if (byte == 0x44) matches += 1;
    }
    try testing.expectEqual(@as(u32, 512), matches);
}

test "erase_blocks probes one block and refuses a card that erases to ones" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.data_fill = 0xFF; // post-erase value is ones on this card
    openWith(&card, .sdhc, 8);

    try testing.expectEqual(err_not_supported, abi.ra8_sdmmc_spi_erase_blocks(0, 4));
    // The probe erased exactly ONE block: CMD32 ran with the first LBA and
    // CMD33 with the same one, never the end of the whole range.
    try testing.expectEqual(@as(u32, 0), frameArg(card.tx_log.items, 0x60).?);
    try testing.expectEqual(@as(u32, 0), frameArg(card.tx_log.items, 0x61).?);
}

test "erase_blocks erases the remaining range once the probe reads back zero" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    card.respond = true;
    card.data_fill = 0x00; // this card erases to zero
    openWith(&card, .sdhc, 8);

    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_erase_blocks(0, 4));
    // Two erase transactions: the one-block probe, then blocks 1..3.
    var cmd38_count: u32 = 0;
    const log = card.tx_log.items;
    for (0..log.len) |i| {
        if (i + 5 < log.len and log[i] == 0x66) cmd38_count += 1;
    }
    try testing.expect(cmd38_count >= 2);
}

test "erase_blocks guards the handle, a zero count and the card bounds" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 8);
    try testing.expectEqual(err_invalid_arg, abi.ra8_sdmmc_spi_erase_blocks(0, 0));
    try testing.expectEqual(err_out_of_range, abi.ra8_sdmmc_spi_erase_blocks(7, 2));
    abi.g_sdmmc_spi_state.initialized = false;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_erase_blocks(0, 1));
}

test "bind_fs_backend fills every operation and a null cookie" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 8);

    var backend: abi.FsBackend = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_bind_fs_backend(&backend));
    try testing.expect(backend.read_block != null);
    try testing.expect(backend.write_block != null);
    try testing.expect(backend.get_capacity != null);
    try testing.expect(backend.erase_blocks != null);
    try testing.expect(backend.ctx == null);

    try testing.expectEqual(err_null_ptr, abi.ra8_sdmmc_spi_bind_fs_backend(null));
    abi.g_sdmmc_spi_state.initialized = false;
    try testing.expectEqual(err_invalid_state, abi.ra8_sdmmc_spi_bind_fs_backend(&backend));
}

test "the fs shims publish geometry and refuse a 64-bit lba" {
    var card = Card.init(testing.allocator);
    defer card.deinit();
    openWith(&card, .sdhc, 2048);

    var backend: abi.FsBackend = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_bind_fs_backend(&backend));

    var count: u64 = 0;
    var size: u32 = 0;
    try testing.expectEqual(err_ok, backend.get_capacity.?(null, &count, &size));
    try testing.expectEqual(@as(u64, 2048), count);
    try testing.expectEqual(@as(u32, 512), size);
    try testing.expectEqual(err_null_ptr, backend.get_capacity.?(null, null, &size));
    try testing.expectEqual(err_null_ptr, backend.get_capacity.?(null, &count, null));

    var block: [512]u8 = @splat(0);
    try testing.expectEqual(err_null_ptr, backend.read_block.?(null, 0, 1, null));
    try testing.expectEqual(err_null_ptr, backend.write_block.?(null, 0, 1, null));
    try testing.expectEqual(
        err_out_of_range,
        backend.read_block.?(null, 0x1_0000_0000, 1, &block),
    );
    try testing.expectEqual(
        err_out_of_range,
        backend.write_block.?(null, 0x1_0000_0000, 1, &block),
    );
    try testing.expectEqual(err_out_of_range, backend.erase_blocks.?(null, 0x1_0000_0000, 1));
    try testing.expectEqual(err_out_of_range, backend.erase_blocks.?(null, 0, 0x1_0000_0000));
}
