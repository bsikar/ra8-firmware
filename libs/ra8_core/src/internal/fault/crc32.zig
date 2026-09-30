//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The self-contained bitwise CRC-32 that protects the crash-log record.
//!
//! A hardware CRC peripheral is deliberately not used: the write path runs
//! from a fault context where no peripheral may be assumed powered or
//! initialised. Table-free as well, so the checksum adds no `.rodata` and
//! touches nothing but the bytes it is handed.

/// Standard reflected CRC-32 (IEEE 802.3 / zlib): the polynomial is applied
/// LSB-first and the seed doubles as the final XOR mask.
pub const params = struct {
    pub const poly: u32 = 0xEDB8_8320;
    pub const seed: u32 = 0xFFFF_FFFF;
    /// Bits consumed per input byte.
    pub const bits_per_byte: u8 = 8;
};

/// CRC-32 of a span. Reads are volatile so a record can be checksummed in
/// its own live storage. An empty span yields 0, matching the C.
pub fn compute(data: []const volatile u8) u32 {
    var crc = params.seed;
    for (data) |byte| {
        crc ^= byte;
        for (0..params.bits_per_byte) |_| {
            // Branchless: 0 - (crc & 1) is 0x00000000 or 0xFFFFFFFF.
            const mask = 0 -% (crc & 1);
            crc = (crc >> 1) ^ (params.poly & mask);
        }
    }
    return crc ^ params.seed;
}
