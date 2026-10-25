//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Option-setting words for the RA8 boot ROM (RA8FW-845): the 32-bit
//! constants the boot ROM reads directly out of MRAM before the Cortex-M85
//! reset vector runs. This unit owns the `.option_setting_*` placement; the
//! former C emitter `ra8_ofs.c` is deleted, so there is exactly one owner.
//! The byte-for-byte section proof against the C emitter (RA8FW-845 step 3)
//! fixed the values before the C path was removed.
//!
//! Placement follows the former C emitter's `RA8_SECTION` exactly: on host
//! builds (UNIT_TEST / RA8_OFF_TARGET / __APPLE__ there) the section
//! attribute is dropped, and the same is true here -- a word only carries its
//! `linksection` on an Arm freestanding image.
//!
//! Every word defaults to `0xFFFFFFFF`, the erased-MRAM / permissive value
//! (IWDT and LVD0 disabled, full flash and TrustZone access, no OTP
//! anti-rollback). Safe for bring-up, not for production; per-app override (RA8FW-845
//! step 2) is live: the app's CMake passes `-Dofs1=` / `-Dofs1_sec=` on the
//! ra8_hal build, mirroring the BSP_CFG_OPTION_SETTING_OFS1(_SEC) define its
//! C build used, and the two words take that value. The override must ride
//! the per-app variant key so two apps never share one archive.
//!
//! Section layout is fixed by `libs/ra8_board_ek_ra8d2/ld/linker_script.ld`
//! (`OFS_CFG` / `OFS_OTP` regions) and pinned at the HUM addresses recorded
//! in `ra8_ofs.h`. The eight `otp_sacc*` sections in that script are
//! deliberately not emitted here or in the C: no value is ever written to
//! OTP words without the byte-for-byte proof.

const builtin = @import("builtin");
const options = @import("ra8_hal_options");

/// True only where the boot ROM actually reads these words out of MRAM: an
/// Arm freestanding image. Host builds keep the values testable but place
/// nothing, matching the C's UNIT_TEST / RA8_OFF_TARGET / __APPLE__ gate.
const on_silicon = builtin.cpu.arch.isArm() and builtin.os.tag == .freestanding;
/// The OFS word inventory from `ra8_ofs.h`: both supported parts carry the
/// identical OFS0 / OFS1 / OFS2 / OFS3 quartet (RA8D2 HUM R01UH1065EJ0130
/// Ch 7.2.1 p 280 / Ch 7.2.6 p 287; RA8P1 HUM R01UH1064EJ0130 Ch 7.2.6
/// p 288), so the count is device-invariant. The remaining section words
/// (SAS, the TrustZone selectors, BPS, the OTP words) sit outside this
/// quartet and are not counted.
pub const ofs_word_count = 4;

pub const Words = if (on_silicon) struct {
    export const ofs0: u32 linksection(".option_setting_ofs0") = 0xFFFFFFFF;
    export const ofs1: u32 linksection(".option_setting_ofs1") = options.ofs1;
    export const ofs2: u32 linksection(".option_setting_ofs2") = 0xFFFFFFFF;
    export const ofs3: u32 linksection(".option_setting_ofs3") = 0xFFFFFFFF;
    export const sas: u32 linksection(".option_setting_sas") = 0xFFFFFFFF;
    export const ofs1_sec: u32 linksection(".option_setting_ofs1_sec") = options.ofs1_sec;
    export const ofs1_sel: u32 linksection(".option_setting_ofs1_sel") = 0xFFFFFFFF;
    export const ofs3_sec: u32 linksection(".option_setting_ofs3_sec") = 0xFFFFFFFF;
    export const ofs3_sel: u32 linksection(".option_setting_ofs3_sel") = 0xFFFFFFFF;
    export const bps: u32 linksection(".option_setting_bps") = 0xFFFFFFFF;
    export const bps_sec: u32 linksection(".option_setting_bps_sec") = 0xFFFFFFFF;
    export const otp_fsblctrl0: u32 linksection(".option_setting_otp_fsblctrl0") = 0xFFFFFFFF;
    export const otp_fsblctrl1: u32 linksection(".option_setting_otp_fsblctrl1") = 0xFFFFFFFF;
    export const otp_fsblctrl2: u32 linksection(".option_setting_otp_fsblctrl2") = 0xFFFFFFFF;
    export const otp_samr: u32 linksection(".option_setting_otp_samr") = 0xFFFFFFFF;
    export const otp_pbps: u32 linksection(".option_setting_otp_pbps") = 0xFFFFFFFF;
    export const otp_pbps_sec: u32 linksection(".option_setting_otp_pbps_sec") = 0xFFFFFFFF;
    export const otp_zhuk: u32 linksection(".option_setting_otp_zhuk") = 0xFFFFFFFF;
} else struct {
    pub const ofs0: u32 = 0xFFFFFFFF;
    pub const ofs1: u32 = options.ofs1;
    pub const ofs2: u32 = 0xFFFFFFFF;
    pub const ofs3: u32 = 0xFFFFFFFF;
    pub const sas: u32 = 0xFFFFFFFF;
    pub const ofs1_sec: u32 = options.ofs1_sec;
    pub const ofs1_sel: u32 = 0xFFFFFFFF;
    pub const ofs3_sec: u32 = 0xFFFFFFFF;
    pub const ofs3_sel: u32 = 0xFFFFFFFF;
    pub const bps: u32 = 0xFFFFFFFF;
    pub const bps_sec: u32 = 0xFFFFFFFF;
    pub const otp_fsblctrl0: u32 = 0xFFFFFFFF;
    pub const otp_fsblctrl1: u32 = 0xFFFFFFFF;
    pub const otp_fsblctrl2: u32 = 0xFFFFFFFF;
    pub const otp_samr: u32 = 0xFFFFFFFF;
    pub const otp_pbps: u32 = 0xFFFFFFFF;
    pub const otp_pbps_sec: u32 = 0xFFFFFFFF;
    pub const otp_zhuk: u32 = 0xFFFFFFFF;
};

comptime {
    // Keep the selected branch analyzed in every build so the exports are
    // emitted into the archive object when the build owns the placement.
    _ = Words;
}

test "every word defaults to the erased-MRAM value" {
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs0);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs1);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs2);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs3);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.sas);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs1_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs1_sel);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs3_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.ofs3_sel);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.bps);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.bps_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_fsblctrl0);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_fsblctrl1);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_fsblctrl2);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_samr);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_pbps);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_pbps_sec);
    try std.testing.expectEqual(@as(u32, 0xFFFFFFFF), Words.otp_zhuk);
}

test "the OFS quartet inventory matches ra8_ofs.h" {
    try std.testing.expectEqual(@as(usize, 4), ofs_word_count);
}

const std = @import("std");
