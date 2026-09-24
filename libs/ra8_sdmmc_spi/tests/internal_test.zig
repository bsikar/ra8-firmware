//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native tests for the pure protocol core of `ra8_sdmmc_spi`: both CRC
//! generators against the SD spec's published constants, command-frame
//! serialization, R1 and R7 decoding, CSD v1 / v2 capacity decoding, card
//! classification and the transport gate.

const std = @import("std");
const core = @import("implementation");
const testing = std.testing;

test "crc7 of a null pointer is zero, as in the C" {
    try testing.expectEqual(@as(u8, 0), core.crc7(null, 5));
    try testing.expectEqual(@as(u8, 0), core.crc7(null, 0));
}

test "crc7 of an empty span is the zero register" {
    const data = [_]u8{ 0x40, 0, 0, 0, 0 };
    try testing.expectEqual(@as(u8, 0), core.crc7(&data, 0));
}

test "crc7 reproduces the spec's CMD0 frame byte" {
    // CMD0 with a zero argument: the spec publishes (CRC7 << 1) | 1 == 0x95.
    const frame = [_]u8{ 0x40, 0, 0, 0, 0 };
    const c = core.crc7(&frame, 5);
    try testing.expectEqual(core.proto.crc7_cmd0_byte, (c << 1) | 1);
}

test "crc7 reproduces the spec's CMD8 frame byte" {
    // CMD8 with argument 0x000001AA: the spec publishes 0x87.
    const frame = [_]u8{ 0x48, 0x00, 0x00, 0x01, 0xAA };
    const c = core.crc7(&frame, 5);
    try testing.expectEqual(core.proto.crc7_cmd8_byte, (c << 1) | 1);
}

test "crc7 keeps the working register inside seven bits" {
    const data = [_]u8{ 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    var n: u32 = 1;
    while (n <= data.len) : (n += 1) {
        try testing.expect(core.crc7(&data, n) <= 0x7F);
    }
}

test "crc16 of a null pointer is zero, as in the C" {
    try testing.expectEqual(@as(u16, 0), core.crc16(null, 512));
}

test "crc16-ccitt matches the classic check vector" {
    const data = "123456789";
    try testing.expectEqual(@as(u16, 0x31C3), core.crc16(data.ptr, data.len));
}

test "crc16 of an all-zero block is zero" {
    const block = [_]u8{0} ** 512;
    try testing.expectEqual(@as(u16, 0), core.crc16(&block, block.len));
}

test "crc16 of a single 0xFF byte" {
    const data = [_]u8{0xFF};
    try testing.expectEqual(@as(u16, 0x1EF0), core.crc16(&data, 1));
}

test "buildFrame lays the argument out big-endian behind the command byte" {
    var frame: [6]u8 = undefined;
    core.buildFrame(core.cmd.set_blocklen, 0x1234_5678, &frame);
    try testing.expectEqual(core.cmd.set_blocklen, frame[0]);
    try testing.expectEqual(@as(u8, 0x12), frame[1]);
    try testing.expectEqual(@as(u8, 0x34), frame[2]);
    try testing.expectEqual(@as(u8, 0x56), frame[3]);
    try testing.expectEqual(@as(u8, 0x78), frame[4]);
}

test "buildFrame uses the spec's constant CRC byte for CMD0" {
    var frame: [6]u8 = undefined;
    core.buildFrame(core.cmd.go_idle_state, 0, &frame);
    try testing.expectEqual(core.proto.crc7_cmd0_byte, frame[5]);
}

test "buildFrame uses the constant CRC byte only for the documented CMD8 argument" {
    var frame: [6]u8 = undefined;
    core.buildFrame(core.cmd.send_if_cond, core.proto.cmd8_arg_check_pattern, &frame);
    try testing.expectEqual(core.proto.crc7_cmd8_byte, frame[5]);

    // Any other CMD8 argument falls through to the computed CRC.
    core.buildFrame(core.cmd.send_if_cond, 0x0000_01AB, &frame);
    const computed = core.crc7(&frame, 5);
    try testing.expectEqual((computed << 1) | 1, frame[5]);
}

test "buildFrame terminates every computed CRC byte with the end bit" {
    var frame: [6]u8 = undefined;
    core.buildFrame(core.cmd.read_single_block, 0xDEAD_BEEF, &frame);
    try testing.expectEqual(@as(u8, 1), frame[5] & 1);
}

test "buildFrame is stable for the same command and argument" {
    var a: [6]u8 = undefined;
    var b: [6]u8 = undefined;
    core.buildFrame(core.cmd.write_multi_block, 0x0000_0040, &a);
    core.buildFrame(core.cmd.write_multi_block, 0x0000_0040, &b);
    try testing.expectEqualSlices(u8, &a, &b);
}

test "isR1 accepts only a byte with the sentinel bit clear" {
    try testing.expect(core.isR1(0x00));
    try testing.expect(core.isR1(0x01));
    try testing.expect(core.isR1(0x7F));
    try testing.expect(!core.isR1(0x80));
    try testing.expect(!core.isR1(0xFF));
}

test "cmd8SaysLegacy fires on the illegal-command bit alone" {
    try testing.expect(core.cmd8SaysLegacy(core.r1.illegal_command));
    try testing.expect(core.cmd8SaysLegacy(core.r1.illegal_command | core.r1.idle_state));
    try testing.expect(!core.cmd8SaysLegacy(core.r1.idle_state));
    try testing.expect(!core.cmd8SaysLegacy(0));
}

test "acmd41Done waits for the idle bit to clear" {
    try testing.expect(core.acmd41Done(0x00));
    try testing.expect(!core.acmd41Done(core.r1.idle_state));
    try testing.expect(core.acmd41Done(core.r1.erase_reset));
}

test "echoMatches checks the low twelve bits of the R7 echo" {
    try testing.expect(core.echoMatches(0x0000_01AA));
    // Upper bits are the card's business and must not matter.
    try testing.expect(core.echoMatches(0xFFFF_F1AA));
    try testing.expect(!core.echoMatches(0x0000_01AB));
    try testing.expect(!core.echoMatches(0x0000_00AA));
}

test "ocrIsHighCapacity reads the CCS bit" {
    try testing.expect(core.ocrIsHighCapacity(core.proto.ocr_ccs_bit));
    try testing.expect(core.ocrIsHighCapacity(0xC0FF_8000));
    try testing.expect(!core.ocrIsHighCapacity(0x8000_0000));
    try testing.expect(!core.ocrIsHighCapacity(0));
}

test "tailWord assembles four bytes big-endian" {
    try testing.expectEqual(
        @as(u32, 0x0102_0304),
        core.tailWord([4]u8{ 0x01, 0x02, 0x03, 0x04 }),
    );
    try testing.expectEqual(@as(u32, 0), core.tailWord([4]u8{ 0, 0, 0, 0 }));
    try testing.expectEqual(
        @as(u32, 0xFFFF_FFFF),
        core.tailWord([4]u8{ 0xFF, 0xFF, 0xFF, 0xFF }),
    );
}

test "csdToBlocks decodes a CSD v2 4 GiB card" {
    // version 1 in byte 0 bits 7:6, C_SIZE = 7679 -> (7679 + 1) * 1024.
    var csd = [_]u8{0} ** 16;
    csd[0] = 0x40;
    csd[7] = 0x00;
    csd[8] = 0x1D;
    csd[9] = 0xFF;
    try testing.expectEqual(@as(u32, 7680 * 1024), core.csdToBlocks(&csd));
}

test "csdToBlocks masks the CSD v2 C_SIZE MSB to six bits" {
    var csd = [_]u8{0} ** 16;
    csd[0] = 0x40;
    csd[7] = 0xFF; // only the low six bits belong to C_SIZE
    csd[8] = 0x00;
    csd[9] = 0x00;
    const c_size: u32 = 0x3F << 16;
    try testing.expectEqual((c_size + 1) << 10, core.csdToBlocks(&csd));
}

test "csdToBlocks decodes a CSD v1 card through the mult/blocklen product" {
    // version 0, C_SIZE = 3751, C_SIZE_MULT = 7, READ_BL_LEN = 9:
    // (3751 + 1) * 512 * 512 / 512 blocks.
    var csd = [_]u8{0} ** 16;
    csd[0] = 0x00;
    csd[5] = 0x09;
    csd[6] = 0x03;
    csd[7] = 0xA5;
    csd[8] = 0xC0;
    csd[9] = 0x03;
    csd[10] = 0x80;
    const c_size: u32 = (0x03 << 10) | (0xA5 << 2) | 0x03;
    const expected: u32 = @truncate(
        (@as(u64, c_size + 1) * @as(u64, 512) * @as(u64, 512)) / 512,
    );
    try testing.expectEqual(expected, core.csdToBlocks(&csd));
}

test "csdToBlocks answers zero for a reserved CSD version" {
    var csd = [_]u8{0} ** 16;
    csd[0] = 0x80; // version 2, reserved
    try testing.expectEqual(@as(u32, 0), core.csdToBlocks(&csd));
    csd[0] = 0xC0; // version 3, reserved
    try testing.expectEqual(@as(u32, 0), core.csdToBlocks(&csd));
}

test "csdToBlocks answers zero for an all-zero CSD v1 register" {
    // c_size 0, mult 4, block_len 1 -> 4 bytes, which is under one block.
    const csd = [_]u8{0} ** 16;
    try testing.expectEqual(@as(u32, 0), core.csdToBlocks(&csd));
}

test "classifyCard prefers high capacity over version" {
    try testing.expectEqual(core.CardType.sdhc, core.classifyCard(true, true));
    try testing.expectEqual(core.CardType.sdhc, core.classifyCard(false, true));
    try testing.expectEqual(core.CardType.sdv2, core.classifyCard(true, false));
    try testing.expectEqual(core.CardType.sdv1, core.classifyCard(false, false));
}

test "validateTransport rejects a null descriptor before a null callback" {
    try testing.expectEqual(core.err.null_ptr, core.validateTransport(null));
}

test "validateTransport rejects each missing callback with invalid_arg" {
    const stub = struct {
        fn setClock(_: ?*anyopaque, _: u32) callconv(.c) u16 {
            return 0;
        }
        fn cs(_: ?*anyopaque, _: bool) callconv(.c) u16 {
            return 0;
        }
        fn xfer(_: ?*anyopaque, _: ?[*]const u8, _: ?[*]u8, _: u32) callconv(.c) u16 {
            return 0;
        }
    };
    const full = core.Transport{
        .set_clock = stub.setClock,
        .cs = stub.cs,
        .xfer = stub.xfer,
        .ctx = null,
    };
    try testing.expectEqual(core.err.ok, core.validateTransport(&full));

    var missing = full;
    missing.set_clock = null;
    try testing.expectEqual(core.err.invalid_arg, core.validateTransport(&missing));

    missing = full;
    missing.cs = null;
    try testing.expectEqual(core.err.invalid_arg, core.validateTransport(&missing));

    missing = full;
    missing.xfer = null;
    try testing.expectEqual(core.err.invalid_arg, core.validateTransport(&missing));
}

test "validateTransport accepts a null context, which is a legal value" {
    const stub = struct {
        fn setClock(_: ?*anyopaque, _: u32) callconv(.c) u16 {
            return 0;
        }
        fn cs(_: ?*anyopaque, _: bool) callconv(.c) u16 {
            return 0;
        }
        fn xfer(_: ?*anyopaque, _: ?[*]const u8, _: ?[*]u8, _: u32) callconv(.c) u16 {
            return 0;
        }
    };
    const t = core.Transport{
        .set_clock = stub.setClock,
        .cs = stub.cs,
        .xfer = stub.xfer,
        .ctx = null,
    };
    try testing.expectEqual(core.err.ok, core.validateTransport(&t));
}

test "wakeIdleBytes rounds the spec's 74-clock floor up to whole bytes" {
    try testing.expectEqual(@as(u32, 10), core.wakeIdleBytes());
    try testing.expect(core.wakeIdleBytes() * 8 >= 74);
}

test "the state object starts unprobed and unbound" {
    const s = core.State{};
    try testing.expectEqual(@as(u8, @intFromEnum(core.CardType.unknown)), s.card_type);
    try testing.expectEqual(@as(u32, 0), s.capacity_blocks);
    try testing.expect(!s.initialized);
    try testing.expect(s.transport.xfer == null);
}

test "protocol budgets are the C's values" {
    try testing.expectEqual(@as(u32, 16), core.proto.max_r1_wait_bytes);
    try testing.expectEqual(@as(u32, 50_000), core.proto.max_data_token_polls);
    try testing.expectEqual(@as(u32, 100_000), core.proto.max_busy_poll_bytes);
    try testing.expectEqual(@as(u32, 1000), core.proto.max_acmd41_attempts);
    try testing.expectEqual(@as(u32, 530), core.proto.recover_flush_bytes);
    try testing.expectEqual(@as(u32, 64), core.proto.recover_idle_bytes);
    try testing.expectEqual(@as(u32, 4), core.proto.max_recover_attempts);
    try testing.expectEqual(@as(u32, 5_000_000), core.proto.max_erase_poll_bytes);
}

test "command wire bytes carry the spec's transmission bits" {
    try testing.expectEqual(@as(u8, 0x40), core.cmd.go_idle_state);
    try testing.expectEqual(@as(u8, 0x48), core.cmd.send_if_cond);
    try testing.expectEqual(@as(u8, 0x51), core.cmd.read_single_block);
    try testing.expectEqual(@as(u8, 0x58), core.cmd.write_single_block);
    try testing.expectEqual(@as(u8, 0x77), core.cmd.app_cmd);
    try testing.expectEqual(@as(u8, 0x7A), core.cmd.read_ocr);
    try testing.expectEqual(@as(u8, 0x69), core.cmd.acmd_sd_send_op_cond);
}

test "data-response classes sit behind the five-bit mask" {
    const accepted = core.data_response.accepted | 0xE0;
    try testing.expectEqual(
        core.data_response.accepted,
        accepted & core.data_response.mask,
    );
    try testing.expect(core.data_response.crc_err != core.data_response.accepted);
    try testing.expect(core.data_response.write_err != core.data_response.accepted);
}

// ---------------------------------------------------------------------------
// Block I/O decisions (ported from `src/ra8_sdmmc_spi_io.c`)
// ---------------------------------------------------------------------------

test "lba_to_arg passes the block number through on a block-addressed card" {
    const sdhc = @intFromEnum(core.CardType.sdhc);
    try testing.expectEqual(@as(u32, 0), core.lbaToArg(sdhc, 0));
    try testing.expectEqual(@as(u32, 1), core.lbaToArg(sdhc, 1));
    try testing.expectEqual(@as(u32, 0x00FF_FFFF), core.lbaToArg(sdhc, 0x00FF_FFFF));
}

test "lba_to_arg converts to a byte offset on a byte-addressed card" {
    for ([_]core.CardType{ .unknown, .sdv1, .sdv2 }) |kind| {
        const t = @intFromEnum(kind);
        try testing.expectEqual(@as(u32, 0), core.lbaToArg(t, 0));
        try testing.expectEqual(@as(u32, 512), core.lbaToArg(t, 1));
        try testing.expectEqual(@as(u32, 1024), core.lbaToArg(t, 2));
    }
}

test "lba_to_arg keeps the C's wrapping multiplication" {
    // Unreachable through the public API (the bounds checks run first), but
    // the C wrapped here rather than trapping, so the port must too.
    const t = @intFromEnum(core.CardType.sdv2);
    try testing.expectEqual(@as(u32, 0), core.lbaToArg(t, 0x0080_0000));
    try testing.expectEqual(@as(u32, 512), core.lbaToArg(t, 0x0080_0001));
}

test "erase_end_lba names the last block of the range" {
    try testing.expectEqual(@as(u32, 0), core.eraseEndLba(0, 1));
    try testing.expectEqual(@as(u32, 9), core.eraseEndLba(0, 10));
    try testing.expectEqual(@as(u32, 109), core.eraseEndLba(100, 10));
}

test "erase_end_lba wraps exactly as the C's uint32 arithmetic did" {
    try testing.expectEqual(@as(u32, 0xFFFF_FFFF), core.eraseEndLba(0, 0));
    try testing.expectEqual(@as(u32, 0xFFFF_FFFE), core.eraseEndLba(0xFFFF_FFFF, 0));
}

test "single-block bounds admit only an existing block" {
    try testing.expect(core.singleBlockInRange(10, 0));
    try testing.expect(core.singleBlockInRange(10, 9));
    try testing.expect(!core.singleBlockInRange(10, 10));
    try testing.expect(!core.singleBlockInRange(0, 0));
}

test "multi-block bounds admit a run that ends inside the card" {
    try testing.expect(core.multiBlockInRange(10, 0, 10));
    try testing.expect(core.multiBlockInRange(10, 5, 5));
    try testing.expect(!core.multiBlockInRange(10, 5, 6));
    try testing.expect(!core.multiBlockInRange(10, 10, 1));
    try testing.expect(!core.multiBlockInRange(0, 0, 1));
}

test "multi-block bounds cannot be defeated by an overflowing lba + count" {
    // The C subtracted on the capacity side precisely so this stays refused
    // instead of wrapping into a legal-looking range.
    try testing.expect(!core.multiBlockInRange(1000, 900, 0xFFFF_FFFF));
    try testing.expect(!core.multiBlockInRange(1000, 0xFFFF_FFF0, 0x20));
}

test "data-response token accepts only the spec's 0b010 verdict" {
    try testing.expect(core.dataResponseAccepted(0x05));
    // The three reserved high bits are don't-care, so a floating one still
    // reads as an accept when the low five bits say so.
    try testing.expect(core.dataResponseAccepted(0xE5));
    try testing.expect(!core.dataResponseAccepted(0x0B)); // CRC error
    try testing.expect(!core.dataResponseAccepted(0x0D)); // write error
    try testing.expect(!core.dataResponseAccepted(0xFF)); // floating bus
    try testing.expect(!core.dataResponseAccepted(0x00));
}

test "crc bytes go out high byte first and come back the same way" {
    try testing.expectEqual([2]u8{ 0x31, 0xC3 }, core.crcBytes(0x31C3));
    try testing.expectEqual([2]u8{ 0x00, 0x00 }, core.crcBytes(0));
    try testing.expectEqual([2]u8{ 0xFF, 0xFF }, core.crcBytes(0xFFFF));
    try testing.expectEqual(@as(u16, 0x31C3), core.crcFromBytes(0x31, 0xC3));
    try testing.expectEqual(@as(u16, 0x00FF), core.crcFromBytes(0x00, 0xFF));
}

test "crc round-trips through the wire byte pair" {
    var value: u32 = 0;
    while (value <= 0xFFFF) : (value += 0x111) {
        const crc: u16 = @truncate(value);
        const pair = core.crcBytes(crc);
        try testing.expectEqual(crc, core.crcFromBytes(pair[0], pair[1]));
    }
}

test "erase probe only claims support when the block came back all zero" {
    var block: [core.block_size]u8 = @splat(0);
    try testing.expect(core.erasedToZero(&block));
    block[core.block_size - 1] = 0xFF;
    try testing.expect(!core.erasedToZero(&block));
    block[core.block_size - 1] = 0;
    block[0] = 0x01;
    try testing.expect(!core.erasedToZero(&block));
}

test "erase probe treats an all-ones card as unsupported" {
    const ones: [core.block_size]u8 = @splat(0xFF);
    try testing.expect(!core.erasedToZero(&ones));
}

test "the fs backend refuses an lba past the 32-bit SD reach" {
    try testing.expect(core.fsLbaFits(0));
    try testing.expect(core.fsLbaFits(0xFFFF_FFFF));
    try testing.expect(!core.fsLbaFits(0x1_0000_0000));
    try testing.expect(!core.fsLbaFits(std.math.maxInt(u64)));
}

test "the transport factory refuses a zero PCLKA rate" {
    try testing.expect(!core.factoryPclkOk(0));
    try testing.expect(core.factoryPclkOk(1));
    try testing.expect(core.factoryPclkOk(60_000_000));
}

test "the io layer's C struct layouts hold on this target" {
    // The comptime asserts in the implementation do the real work; this
    // pins the sizes a reader would otherwise have to take on trust.
    try testing.expectEqual(@as(usize, 8), @sizeOf(core.SciPins));
    try testing.expectEqual(@as(usize, 12), @sizeOf(core.SciSpiCfg));
    try testing.expectEqual(@sizeOf(usize) * 5, @sizeOf(core.FsBackend));
}
