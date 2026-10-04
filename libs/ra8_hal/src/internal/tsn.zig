//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! On-die temperature sensor maths (HUM Ch 55). Pure helpers; the C ABI
//! and register access live in tsn_abi.zig (RA8FW-577).

pub const ctrl_base: usize = 0x40235000;
pub const cal_base: usize = 0x02C1EDA0;

pub const tscr_tsen: u8 = 1 << 4;
pub const tscr_tsoe: u8 = 1 << 7;
pub const tscr_all: u8 = tscr_tsen | tscr_tsoe;
pub const code_mask: u16 = 0x0FFF;

pub const temp_high_125: i16 = 125;
pub const temp_high_105: i16 = 105;
pub const temp_low_n40: i16 = -40;
/// tTSTBL floor, HUM Ch 55.3.2 Figure 55.2.
pub const min_stab_us: u16 = 30;
pub const busy_loops_per_us: u16 = 1000;

pub const avcc_uv: i64 = 3_300_000;
pub const full_scale: i64 = 4096;
pub const uv_per_mv: i64 = 1000;

/// MSTPD22 (k_ra8_mstp_reg_d = 3).
pub const mstp_id: u16 = (3 << 8) | 22;
/// k_ra8_adc_chan_temperature (CNVCS = 0x64).
pub const adc_chan_temperature: u8 = 0x64;

/// Mirror of ra8_tsn_config_t (ra8_tsn_cal_temp_t is enum : int16_t).
pub const Config = extern struct {
    high_ref_degc: i16,
    low_ref_degc: i16,
    stab_us: u16,
};

pub fn configOk(cfg: Config) bool {
    const high_ok = cfg.high_ref_degc == temp_high_125 or cfg.high_ref_degc == temp_high_105;
    return high_ok and cfg.low_ref_degc == temp_low_n40 and cfg.stab_us >= min_stab_us;
}

pub fn maskCode(raw: u16) u16 {
    return raw & code_mask;
}

fn toUv(code: u32) i64 {
    return @divTrunc(avcc_uv * @as(i64, code), full_scale);
}

/// Two-point line through the factory trim (HUM 55.3.1), in milli-degC.
/// Null when both trim codes match, so no slope is computable.
pub fn convert(raw: u16, cal_hi_word: u32, cal_lo_word: u32, high_degc: i16, low_degc: i16) ?i32 {
    const cal_hi = cal_hi_word & code_mask;
    const cal_lo = cal_lo_word & code_mask;
    if (cal_hi == cal_lo) return null;
    const v1 = toUv(cal_hi);
    const v2 = toUv(cal_lo);
    const vs = toUv(raw);
    const t1 = @as(i64, high_degc) * uv_per_mv;
    const t2 = @as(i64, low_degc) * uv_per_mv;
    const result = @divTrunc((vs - v1) * (t1 - t2), v1 - v2) + t1;
    return @truncate(result);
}
