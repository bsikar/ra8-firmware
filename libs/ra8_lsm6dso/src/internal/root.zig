//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Pure core of the ST LSM6DSO 6-DoF IMU driver: register addresses, the
//! CTRL1_XL / CTRL2_G bit-field encoders, the little-endian sample decoders,
//! the temperature conversion and the FIFO depth arithmetic.
//!
//! Nothing here touches the transport. Every register citation points at
//! LSM6DSO DS12140 Rev 4 (Sept 2019), exactly as the C implementation did.

const std = @import("std");

/// Three-axis raw sample (`ra8_lsm6dso_xyz_t`).
pub const Xyz = extern struct {
    x: i16,
    y: i16,
    z: i16,
};

// Register addresses this driver touches (DS12140 sec 9.x).
pub const reg_who_am_i: u8 = 0x0F;
pub const reg_ctrl1_xl: u8 = 0x10;
pub const reg_ctrl2_g: u8 = 0x11;
pub const reg_out_temp_l: u8 = 0x20;
pub const reg_outx_l_g: u8 = 0x22;
pub const reg_outx_l_a: u8 = 0x28;
pub const reg_fifo_status1: u8 = 0x3A;
pub const reg_fifo_data_out: u8 = 0x78;

/// WHO_AM_I reply of a genuine LSM6DSO (DS12140 sec 9.11).
pub const who_am_i_value: u8 = 0x6C;

// Field masks and shifts shared by CTRL1_XL (sec 9.12) and CTRL2_G (sec 9.13).
pub const mask_odr: u8 = 0xF0;
pub const mask_fs_xl: u8 = 0x0C;
pub const mask_fs_g_full: u8 = 0x0E;
pub const mask_nibble: u8 = 0x0F;
pub const shift_odr: u3 = 4;
pub const shift_fs_xl: u3 = 2;
pub const shift_fs_g: u3 = 2;
pub const shift_fs_125: u3 = 1;

// Highest accepted enumerator of each configuration enum.
pub const xl_fs_cap: u8 = 0x03; // k_lsm6dso_xl_fs_8g
pub const g_fs_cap: u8 = 0x04; // k_lsm6dso_g_fs_2000dps
pub const odr_cap: u8 = 0x0A; // k_lsm6dso_odr_6660hz

// Gyro full-scale enumerators that the FS encoder branches on.
pub const g_fs_125dps: u8 = 0x00;
pub const g_fs_250dps: u8 = 0x01;

// Burst sizes.
pub const xyz_burst_bytes: u32 = 6; // 3 axes * 2 bytes (sec 9.29 / 9.35)
pub const temp_burst_bytes: u32 = 2; // OUT_TEMP_L + OUT_TEMP_H (sec 9.27)
pub const fifo_status_bytes: u32 = 2; // FIFO_STATUS1 + FIFO_STATUS2 (sec 9.44)
pub const fifo_bytes_word: u32 = 7; // TAG + 6 sample bytes (sec 9.7)

// Temperature conversion, DS12140 sec 4.3: T[degC] = raw / 256 + 25.
pub const temp_offset_centi_c: i32 = 2500;
pub const temp_scale_num: i32 = 100;
pub const temp_scale_den: i32 = 256;

/// Compose the FS_G + FS_125 sub-field [3:1] of CTRL2_G, pre-shifted.
///
/// DS12140 Table 47: FS_125 (bit 1) wins outright and leaves FS_G[1:0] a
/// don't-care; the wider scales map 250/500/1000/2000 dps onto FS_G 0..3 by
/// subtracting the 125 dps slot.
pub fn gyroFsBits(fs: u8) u8 {
    if (fs == g_fs_125dps) {
        return @as(u8, 1) << shift_fs_125;
    }
    const fs_g_field: u8 = fs -% g_fs_250dps;
    return (fs_g_field & 0x03) << shift_fs_g;
}

/// Merge an accel full-scale code into a live CTRL1_XL byte (FS_XL[3:2]).
pub fn accelFsMerge(current: u8, fs: u8) u8 {
    return (current & ~mask_fs_xl) | ((fs & 0x03) << shift_fs_xl);
}

/// Merge a gyro full-scale code into a live CTRL2_G byte (FS_G + FS_125).
pub fn gyroFsMerge(current: u8, fs: u8) u8 {
    return (current & ~mask_fs_g_full) | gyroFsBits(fs);
}

/// Pre-shift an ODR code into bits [7:4].
pub fn odrBits(odr: u8) u8 {
    return (odr & mask_nibble) << shift_odr;
}

/// Merge a pre-shifted ODR nibble into a live CTRL byte, keeping [3:0].
pub fn odrMerge(current: u8, odr_bits: u8) u8 {
    return (current & ~mask_odr) | odr_bits;
}

/// Combine a little-endian two's-complement pair into a signed sample.
pub fn combineLe(low: u8, high: u8) i16 {
    const raw: u16 = (@as(u16, high) << 8) | @as(u16, low);
    return @bitCast(raw);
}

/// Unpack a 6-byte XYZ burst (X_L, X_H, Y_L, Y_H, Z_L, Z_H).
pub fn unpackXyz(bytes: *const [6]u8) Xyz {
    return .{
        .x = combineLe(bytes[0], bytes[1]),
        .y = combineLe(bytes[2], bytes[3]),
        .z = combineLe(bytes[4], bytes[5]),
    };
}

/// Convert a raw OUT_TEMP sample into centi-degrees Celsius.
///
/// Integer division truncates toward zero, matching the C's `/` on int32.
pub fn tempCentiC(raw: i16) i32 {
    return @divTrunc(@as(i32, raw) * temp_scale_num, temp_scale_den) + temp_offset_centi_c;
}

/// Extract DIFF_FIFO[9:0] from FIFO_STATUS1 + FIFO_STATUS2 (sec 9.44 / 9.45).
pub fn fifoDepth(status1: u8, status2: u8) u32 {
    return @as(u32, status1) | (@as(u32, status2 & mask_nibble) << 8);
}

/// Clamp the live FIFO depth to the caller's word cap.
pub fn wordsToRead(live: u32, max_words: u32) u32 {
    return if (live > max_words) max_words else live;
}

/// Byte span of `n_words` FIFO records. Wrapping matches the C's uint32_t
/// multiply; the 10-bit depth field caps the real value at 7161 bytes.
pub fn fifoTotalBytes(n_words: u32) u32 {
    return n_words *% fifo_bytes_word;
}
