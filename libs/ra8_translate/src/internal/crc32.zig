//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Compact IEEE 802.3 CRC-32 helper for container integrity verification.

const poly: u32 = 0xEDB88320;

fn stepByte(crc_in: u32, byte: u8) u32 {
    var crc = crc_in ^ byte;
    var bit: u4 = 0;
    while (bit < 8) : (bit += 1) {
        const lsb = crc & 1;
        crc >>= 1;
        if (lsb != 0) crc ^= poly;
    }
    return crc;
}

/// Compute standard IEEE 802.3 CRC-32 over a byte slice.
pub fn crc32(bytes: []const u8) u32 {
    var crc: u32 = 0xFFFF_FFFF;
    for (bytes) |byte| {
        crc = stepByte(crc, byte);
    }
    return ~crc;
}

/// Compute IEEE 802.3 CRC-32 over `bytes`, treating `[zero_start .. zero_start + zero_len)`
/// as zero bytes (used to hash headers carrying an embedded CRC-32 field).
pub fn crc32WithZeroWindow(bytes: []const u8, zero_start: usize, zero_len: usize) u32 {
    var crc: u32 = 0xFFFF_FFFF;
    const zero_end = zero_start + zero_len;
    for (bytes, 0..) |raw_byte, i| {
        const byte = if (i >= zero_start and i < zero_end) 0 else raw_byte;
        crc = stepByte(crc, byte);
    }
    return ~crc;
}
