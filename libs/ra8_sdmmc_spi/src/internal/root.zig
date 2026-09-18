//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure protocol core of the SPI-mode SD card driver (`ra8_sdmmc_spi`),
//! ported from `src/ra8_sdmmc_spi.c`. Everything here is a total function
//! over bytes: the two CRC generators, command-frame serialization, R1
//! decoding, CSD capacity decoding and card classification. No transport
//! callback and no logging reaches this file, so every rule below is
//! testable without a bus.
//!
//! References are to SD Specification Part 1 Physical Layer Simplified
//! Specification v9.10 section 7 ("SPI Mode"), the same citations the C
//! carried inline.

const std = @import("std");

/// `ra8_err_t` values this module reproduces. The C ABI is the numbers.
pub const err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const not_initialized: u16 = 0x10F;
    pub const hw_init_failed: u16 = 0x201;
    pub const hw_timeout: u16 = 0x203;
    pub const protocol_error: u16 = 0x406;
    pub const null_ptr: u16 = 0x504;
};

/// Public limits from `inc/ra8_sdmmc_spi.h`.
pub const block_size: u32 = 512;
pub const cmd_frame_bytes: u32 = 6;
pub const csd_response_len: u32 = 16;

pub const clock_init_hz: u32 = 400_000;
pub const clock_data_hz: u32 = 25_000_000;

/// Wire byte per command: `0x40 | index` (spec 7.3.1.1).
pub const cmd = struct {
    pub const go_idle_state: u8 = 0x40;
    pub const send_if_cond: u8 = 0x40 | 8;
    pub const send_csd: u8 = 0x40 | 9;
    pub const send_cid: u8 = 0x40 | 10;
    pub const stop_transmission: u8 = 0x40 | 12;
    pub const set_blocklen: u8 = 0x40 | 16;
    pub const read_single_block: u8 = 0x40 | 17;
    pub const read_multi_block: u8 = 0x40 | 18;
    pub const write_single_block: u8 = 0x40 | 24;
    pub const write_multi_block: u8 = 0x40 | 25;
    pub const erase_wr_blk_start: u8 = 0x40 | 32;
    pub const erase_wr_blk_end: u8 = 0x40 | 33;
    pub const erase: u8 = 0x40 | 38;
    pub const app_cmd: u8 = 0x40 | 55;
    pub const read_ocr: u8 = 0x40 | 58;
    pub const acmd_sd_send_op_cond: u8 = 0x40 | 41;
    pub const acmd_set_wr_blk_erase_count: u8 = 0x40 | 23;
};

/// Data tokens (spec 7.3.3).
pub const token = struct {
    pub const data_start_single: u8 = 0xFE;
    pub const data_start_multi: u8 = 0xFC;
    pub const stop_multi: u8 = 0xFD;
    pub const idle: u8 = 0xFF;
};

/// Data-response token layout `xxx0sss1` (spec 7.3.3.1).
pub const data_response = struct {
    pub const mask: u8 = 0x1F;
    pub const accepted: u8 = 0x05;
    pub const crc_err: u8 = 0x0B;
    pub const write_err: u8 = 0x0D;
};

/// Magic arguments and retry budgets, verbatim from the C enum.
pub const proto = struct {
    pub const crc7_cmd0_byte: u8 = 0x95;
    pub const cmd8_arg_check_pattern: u32 = 0x0000_01AA;
    pub const crc7_cmd8_byte: u8 = 0x87;
    pub const acmd41_arg_hcs: u32 = 0x4000_0000;
    pub const ocr_ccs_bit: u32 = 0x4000_0000;
    pub const ocr_busy_bit: u32 = 0x8000_0000;
    pub const max_r1_wait_bytes: u32 = 16;
    pub const max_data_token_polls: u32 = 50_000;
    pub const max_busy_poll_bytes: u32 = 100_000;
    pub const max_acmd41_attempts: u32 = 1000;
    pub const init_dummy_clocks: u32 = 80;
    pub const recover_flush_bytes: u32 = 530;
    pub const recover_idle_bytes: u32 = 64;
    pub const max_recover_attempts: u32 = 4;
    pub const max_erase_poll_bytes: u32 = 5_000_000;
};

/// R1 response bits (spec 7.3.2.1). Bit 7 is always clear on a real R1,
/// which is what makes it usable as the poll sentinel.
pub const r1 = struct {
    pub const idle_state: u8 = 0x01;
    pub const erase_reset: u8 = 0x02;
    pub const illegal_command: u8 = 0x04;
    pub const com_crc_error: u8 = 0x08;
    pub const erase_sequence_error: u8 = 0x10;
    pub const address_error: u8 = 0x20;
    pub const parameter_error: u8 = 0x40;
    pub const sentinel: u8 = 0x80;
};

const mask_byte: u32 = 0xFF;
const mask_12bit: u32 = 0xFFF;
const cmd_frame_len: u32 = 5;

/// `ra8_sdmmc_spi_card_type_t`.
pub const CardType = enum(u8) {
    unknown = 0,
    sdv1 = 1,
    sdv2 = 2,
    sdhc = 3,
};

/// `ra8_sdmmc_spi_transport_t`: three callbacks then the caller context.
pub const Transport = extern struct {
    set_clock: ?*const fn (?*anyopaque, u32) callconv(.c) u16 = null,
    cs: ?*const fn (?*anyopaque, bool) callconv(.c) u16 = null,
    xfer: ?*const fn (?*anyopaque, ?[*]const u8, ?[*]u8, u32) callconv(.c) u16 = null,
    ctx: ?*anyopaque = null,
};

/// `sd_state_t` from `src/ra8_sdmmc_spi_internal.h`. The block-I/O TU still
/// reads and writes this object as C, and two host suites reach
/// `g_sdmmc_spi_state.transport` directly, so the layout is load-bearing.
pub const State = extern struct {
    transport: Transport = .{},
    card_type: u8 = @intFromEnum(CardType.unknown),
    capacity_blocks: u32 = 0,
    initialized: bool = false,
};

comptime {
    const ptr = @sizeOf(usize);
    std.debug.assert(@sizeOf(Transport) == ptr * 4);
    std.debug.assert(@offsetOf(Transport, "set_clock") == 0);
    std.debug.assert(@offsetOf(Transport, "cs") == ptr);
    std.debug.assert(@offsetOf(Transport, "xfer") == ptr * 2);
    std.debug.assert(@offsetOf(Transport, "ctx") == ptr * 3);

    std.debug.assert(@offsetOf(State, "transport") == 0);
    std.debug.assert(@offsetOf(State, "card_type") == ptr * 4);
    // `capacity_blocks` realigns to 4 after the one-byte card type.
    std.debug.assert(@offsetOf(State, "capacity_blocks") ==
        std.mem.alignForward(usize, ptr * 4 + 1, 4));
    std.debug.assert(@offsetOf(State, "initialized") ==
        @offsetOf(State, "capacity_blocks") + 4);
    std.debug.assert(@sizeOf(State) ==
        std.mem.alignForward(usize, @offsetOf(State, "initialized") + 1, ptr));
}

/// CRC7 over `data`, generator `G(x) = x^7 + x^3 + 1` (spec 4.5), returned
/// as a 7-bit value in the low bits. A null pointer answers 0, exactly as
/// the C did, so a caller that never checked still gets a defined byte.
pub fn crc7(data: ?[*]const u8, len: u32) u8 {
    const bytes = data orelse return 0;
    var crc: u8 = 0;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        var byte = bytes[i];
        var b: u8 = 0;
        while (b < 8) : (b += 1) {
            const top = (crc & 0x40) ^ ((byte & 0x80) >> 1);
            crc = (crc << 1) & 0x7F;
            if (top != 0) crc ^= 0x09;
            byte <<= 1;
        }
    }
    return crc;
}

/// CRC16-CCITT over `data`, generator `0x1021` with the implicit x^16 term
/// (spec 4.5). Null answers 0, as in the C.
pub fn crc16(data: ?[*]const u8, len: u32) u16 {
    const bytes = data orelse return 0;
    var crc: u16 = 0;
    var i: u32 = 0;
    while (i < len) : (i += 1) {
        crc ^= @as(u16, bytes[i]) << 8;
        var b: u8 = 0;
        while (b < 8) : (b += 1) {
            if ((crc & 0x8000) != 0) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc <<= 1;
            }
        }
    }
    return crc;
}

/// Serialize one command into the six-byte wire frame: command byte,
/// big-endian argument, then `(CRC7 << 1) | 1`. CMD0 and CMD8-with-the-
/// documented-argument use the spec's published constant CRC bytes rather
/// than running the generator, which is why a CMD8 with any other argument
/// still gets a computed CRC.
pub fn buildFrame(command: u8, arg: u32, out_frame: *[6]u8) void {
    out_frame[0] = command;
    out_frame[1] = @truncate((arg >> 24) & mask_byte);
    out_frame[2] = @truncate((arg >> 16) & mask_byte);
    out_frame[3] = @truncate((arg >> 8) & mask_byte);
    out_frame[4] = @truncate(arg & mask_byte);
    if (command == cmd.go_idle_state) {
        out_frame[5] = proto.crc7_cmd0_byte;
    } else if (command == cmd.send_if_cond and arg == proto.cmd8_arg_check_pattern) {
        out_frame[5] = proto.crc7_cmd8_byte;
    } else {
        const c = crc7(out_frame, cmd_frame_len);
        out_frame[5] = (c << 1) | 1;
    }
}

/// True when a polled byte is a real R1 token (top bit clear).
pub fn isR1(byte: u8) bool {
    return (byte & r1.sentinel) == 0;
}

/// CMD8's R7 echo check: the low 12 bits must repeat the voltage-range code
/// and host check pattern we sent (spec 7.3.2.6).
pub fn echoMatches(echo: u32) bool {
    return (echo & mask_12bit) == (proto.cmd8_arg_check_pattern & mask_12bit);
}

/// A card that answers CMD8 with "illegal command" is a v1.x card.
pub fn cmd8SaysLegacy(response: u8) bool {
    return (response & r1.illegal_command) != 0;
}

/// ACMD41 is done once the idle bit clears.
pub fn acmd41Done(response: u8) bool {
    return (response & r1.idle_state) == 0;
}

/// CCS = 1 in the OCR means a block-addressed SDHC/SDXC card.
pub fn ocrIsHighCapacity(ocr: u32) bool {
    return (ocr & proto.ocr_ccs_bit) != 0;
}

/// Assemble the four-byte big-endian tail of an R3 or R7 response.
pub fn tailWord(bytes: [4]u8) u32 {
    return (@as(u32, bytes[0]) << 24) | (@as(u32, bytes[1]) << 16) |
        (@as(u32, bytes[2]) << 8) | @as(u32, bytes[3]);
}

/// Decode the 16-byte CSD register into a 512-byte block count.
///
/// CSD v2 (`version == 1`) carries a 22-bit C_SIZE across bytes 7..9 and the
/// count is `(C_SIZE + 1) * 1024`. CSD v1 (`version == 0`) multiplies
/// C_SIZE, C_SIZE_MULT and READ_BL_LEN out in 64-bit and divides by the
/// block size. Any other version answers 0, which the caller treats as a
/// protocol error.
pub fn csdToBlocks(csd: *const [16]u8) u32 {
    const version: u8 = (csd[0] >> 6) & 0x03;
    if (version == 1) {
        const c_size: u32 = ((@as(u32, csd[7]) & 0x3F) << 16) |
            (@as(u32, csd[8]) << 8) | @as(u32, csd[9]);
        return (c_size +% 1) << 10;
    }
    if (version == 0) {
        const read_bl_len: u5 = @truncate(csd[5] & 0x0F);
        const c_size: u32 = ((@as(u32, csd[6]) & 0x03) << 10) |
            (@as(u32, csd[7]) << 2) | ((@as(u32, csd[8]) & 0xC0) >> 6);
        const c_size_mult: u8 = ((csd[9] & 0x03) << 1) | ((csd[10] & 0x80) >> 7);
        const mult: u32 = @as(u32, 1) << @as(u5, @truncate(@as(u32, c_size_mult) + 2));
        const block_len: u32 = @as(u32, 1) << read_bl_len;
        const bytes: u64 = @as(u64, c_size + 1) * @as(u64, mult) * @as(u64, block_len);
        return @truncate(bytes / @as(u64, block_size));
    }
    return 0;
}

/// Card class from the two probe verdicts. High capacity wins over v2,
/// and a card that answered CMD8 at all is at least v2.
pub fn classifyCard(is_v2: bool, is_hc: bool) CardType {
    if (is_hc) return .sdhc;
    if (is_v2) return .sdv2;
    return .sdv1;
}

/// Transport-descriptor gate: a null descriptor is `null_ptr`, a descriptor
/// with any null callback is `invalid_arg`. The order is the contract; the
/// host suites tell the two apart by which code comes back.
pub fn validateTransport(transport: ?*const Transport) u16 {
    const t = transport orelse return err.null_ptr;
    if (t.set_clock == null or t.cs == null or t.xfer == null) {
        return err.invalid_arg;
    }
    return err.ok;
}

/// Idle bytes the wake sequence clocks: 80 clocks is the spec floor of 74
/// rounded up to whole bytes.
pub fn wakeIdleBytes() u32 {
    return proto.init_dummy_clocks / 8;
}
