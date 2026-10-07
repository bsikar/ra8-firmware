//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the `ra8_sdmmc_spi` SCI transport factory and its
//! block I/O paths, driven over the scripted fake card and fake HAL in
//! `sd_fake.zig`. They pin the pin-routing order, the first-failure behaviour,
//! every factory shim, and the read, write and erase command sequences.

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
const err_hal = fake.err_hal;

const err_invalid_state: u16 = 0x104;
const err_not_supported: u16 = 0x107;
const err_out_of_range: u16 = 0x208;
const err_crc_mismatch: u16 = 0x405;

const pins: abi.SciPins = .{ .sck = 0x0102, .cipo = 0x0103, .copi = 0x0104, .cs = 0x0105 };

/// Bring the driver up as initialised without running the identification
/// sequence: the read/write/erase paths only read `capacity_blocks` and
/// `card_type`, so setting them directly keeps each test to one subject.
fn openWith(card: *Card, card_type: abi.CardType, capacity: u32) void {
    bind(card);
    abi.g_sdmmc_spi_state.card_type = @backingInt(card_type);
    abi.g_sdmmc_spi_state.capacity_blocks = capacity;
    abi.g_sdmmc_spi_state.initialized = true;
}

test "the factory routes all three signal pins then claims CS high" {
    fake.g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_transport_sci(3, 60_000_000, &pins, &transport));

    try testing.expectEqual(@as(usize, 3), fake.g_hal.routed_len);
    try testing.expectEqual(@as(u32, pins.sck), fake.g_hal.routed[0][0]);
    try testing.expectEqual(@as(u32, pins.cipo), fake.g_hal.routed[1][0]);
    try testing.expectEqual(@as(u32, pins.copi), fake.g_hal.routed[2][0]);
    for (fake.g_hal.routed[0..3]) |entry| try testing.expectEqual(@as(u32, 0x04), entry[1]);

    // CS is a plain GPIO output, parked high (deselected).
    try testing.expectEqual([2]u32{ pins.cs, 1 }, fake.g_hal.output_init.?);

    // The channel comes up at the mandatory init clock, mode 0, MSB first.
    try testing.expectEqual(@as(u8, 3), fake.g_hal.sci_init_channel.?);
    const cfg = fake.g_hal.sci_init_cfg.?;
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
    fake.g_hal = .{};
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
    try testing.expectEqual(@as(usize, 0), fake.g_hal.routed_len);
}

test "the factory stops at the first pin that will not route" {
    fake.g_hal = .{};
    fake.g_hal.route_fail_on = 1; // CIPO refuses.
    var transport: abi.Transport = .{};
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expectEqual(@as(usize, 1), fake.g_hal.routed_len);
    try testing.expect(fake.g_hal.output_init == null);
    try testing.expect(fake.g_hal.sci_init_channel == null);
    // The out descriptor is left untouched on the failure path.
    try testing.expect(transport.xfer == null);
}

test "the factory propagates a CS claim failure and an SCI bring-up failure" {
    var transport: abi.Transport = .{};
    fake.g_hal = .{};
    fake.g_hal.output_init_fail = true;
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expect(fake.g_hal.sci_init_channel == null);

    fake.g_hal = .{};
    fake.g_hal.sci_init_fail = true;
    try testing.expectEqual(
        err_hal,
        abi.ra8_sdmmc_spi_transport_sci(0, 60_000_000, &pins, &transport),
    );
    try testing.expectEqual(@as(usize, 3), fake.g_hal.routed_len);
}

test "the factory's shims reach the SCI channel and the CS pin" {
    fake.g_hal = .{};
    var transport: abi.Transport = .{};
    try testing.expectEqual(err_ok, abi.ra8_sdmmc_spi_transport_sci(7, 48_000_000, &pins, &transport));

    try testing.expectEqual(err_ok, transport.set_clock.?(transport.ctx, 25_000_000));
    try testing.expectEqual([3]u32{ 7, 25_000_000, 48_000_000 }, fake.g_hal.set_clock_calls[0]);

    // Active-low select: asserted drives the pin low.
    try testing.expectEqual(err_ok, transport.cs.?(transport.ctx, true));
    try testing.expectEqual(err_ok, transport.cs.?(transport.ctx, false));
    try testing.expectEqual([2]u32{ pins.cs, 0 }, fake.g_hal.gpio_writes[0]);
    try testing.expectEqual([2]u32{ pins.cs, 1 }, fake.g_hal.gpio_writes[1]);

    var byte: u8 = 0xAA;
    try testing.expectEqual(err_ok, transport.xfer.?(transport.ctx, @ptrCast(&byte), null, 1));
    try testing.expectEqual(@as(u8, 7), fake.g_hal.xfer_channel.?);
    try testing.expectEqual(@as(u32, 1), fake.g_hal.xfer_len);
}

test "every factory shim refuses a null bus context" {
    fake.g_hal = .{};
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
        @backingInt(abi.CardType.unknown),
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
    try testing.expectEqual(@backingInt(abi.CardType.sdhc), kind);

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
