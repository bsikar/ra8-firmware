//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Software CRC32 (IEEE 802.3, reflected) over a byte slice.
//!
//! Bitwise rather than table-driven on purpose: the bootloader runs before
//! any RAM image is up, and a 1 KiB table costs more than the handful of
//! microseconds the fold costs over a 448 KiB slot.

/// Reflected-CRC32 parameters. The same value `crc32`/`zlib` produce, so a
/// host tool can pre-compute the `img_crc32` an image header carries.
pub const params = struct {
    /// Shift-register preset.
    pub const init: u32 = 0xFFFF_FFFF;
    /// Reflected form of poly 0x04C1_1DB7.
    pub const poly: u32 = 0xEDB8_8320;
    /// Final output XOR.
    pub const xout: u32 = 0xFFFF_FFFF;
    /// Bits folded per input byte.
    pub const bits_per_byte = 8;
};

/// CRC32 of `data`. An empty slice folds to zero.
pub fn compute(data: []const u8) u32 {
    var crc: u32 = params.init;
    for (data) |byte| {
        crc ^= byte;
        for (0..params.bits_per_byte) |_| {
            crc = if (crc & 1 != 0) (crc >> 1) ^ params.poly else crc >> 1;
        }
    }
    return crc ^ params.xout;
}
