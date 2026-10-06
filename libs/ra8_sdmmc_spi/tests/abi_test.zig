//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `ra8_sdmmc_spi` core ABI membrane and the card
//! identification sequence, driven over the scripted fake transport in
//! `sd_fake.zig`. They pin the command order, the CS discipline around each
//! command, the bounded polls, and the published card type and capacity.

const std = @import("std");
const abi = @import("abi");
const testing = std.testing;
const fake = @import("sd_fake.zig");

const Card = fake.Card;
const bind = fake.bind;
const idle = fake.idle;
const err_ok = fake.err_ok;
const err_invalid_arg = fake.err_invalid_arg;
const err_hw_init_failed = fake.err_hw_init_failed;
const err_hw_timeout = fake.err_hw_timeout;
const err_protocol_error = fake.err_protocol_error;
const err_null_ptr = fake.err_null_ptr;
const err_transport = fake.err_transport;
const bindResponder = fake.bindResponder;
const cardSetClock = fake.cardSetClock;
const cardCs = fake.cardCs;
const cardXfer = fake.cardXfer;

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
    card.csd = @splat(0);
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
    var csd: [16]u8 = @splat(0);
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

test "only CMD9 and CMD16 failures log, and they log under the SDSPI tag" {
    fake.log_lines = 0;

    // A probe that fails before CMD9 logs nothing at all.
    var first = Card.init(testing.allocator);
    defer first.deinit();
    first.answers_r1 = false;
    first.respond = true;
    bind(&first);
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 0), fake.log_lines);

    // A CSD that decodes to zero blocks logs the message then the code.
    var second = Card.init(testing.allocator);
    defer second.deinit();
    second.csd = @splat(0);
    second.respond = true;
    bind(&second);
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 2), fake.log_lines);
    try testing.expectEqualStrings("SDSPI", std.mem.span(fake.last_tag));
    try testing.expectEqual(@as(u32, err_protocol_error), fake.last_value);

    // A SET_BLOCKLEN refusal logs its own message under the same tag.
    var third = Card.init(testing.allocator);
    defer third.deinit();
    third.cmd16_r1 = 0x40;
    third.respond = true;
    bind(&third);
    fake.log_lines = 0;
    _ = abi.priv_sdmmc_spi_run_init_sequence();
    try testing.expectEqual(@as(u32, 2), fake.log_lines);
    try testing.expectEqualStrings("SDSPI", std.mem.span(fake.last_tag));
}
