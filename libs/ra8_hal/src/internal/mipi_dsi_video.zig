//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! MIPI DSI-2 video-mode surface (RA8FW-652): video configure / start /
//! stop and the compact set_video_timing wrapper, ported from
//! ra8_mipi_dsi_dispatch.c. Registers, logging and the bounded status poll
//! go through a `dsi` ops value (offsets from the DSI base) so host tests
//! can use a fake.

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const null_ptr: u16 = 0x504;

pub const off_vmset0r: u16 = 0x400;
pub const off_vmset1r: u16 = 0x404;
pub const off_vmsr: u16 = 0x410;
pub const off_vmscr: u16 = 0x414;
pub const off_vmppsetr: u16 = 0x420;
pub const off_vmvssetr: u16 = 0x428;
pub const off_vmvpsetr: u16 = 0x42C;
pub const off_vmhssetr: u16 = 0x430;
pub const off_vmhpsetr: u16 = 0x434;

pub const vmset0_vstart: u32 = 1 << 0;
pub const vmset0_vstop: u32 = 1 << 1;
pub const vmset0_hsanolp: u32 = 1 << 8;
pub const vmset0_hbpnolp: u32 = 1 << 9;
pub const vmset0_hfpnolp: u32 = 1 << 10;
pub const vmsr_virdy: u32 = 1 << 4;
pub const vmsr_stop: u32 = 1 << 8;
pub const vmsr_clear_all: u32 = 0x00D0_0111;

const vmset1_dly_mask: u32 = 0x3FFC;
const vmpp_txesync: u32 = 1 << 15;
const vmpp_dt_mask: u32 = 0x3F << 16;
const vmpp_vc_mask: u32 = 0x3 << 22;
const sync_mask: u32 = 0x0FFF;
const porch_mask: u32 = 0x1FFF;
const active_hi_mask: u32 = 0x7FFF << 16;
const porch_hi_mask: u32 = 0x1FFF << 16;
const pol_bit: u32 = 1 << 15;

/// `k_ra8_mipi_dsi_vc3`, the highest virtual channel.
pub const vc_max: u8 = 3;
/// `k_ra8_mipi_dsi_dt_pixel_rgb888`.
pub const dt_rgb888: u8 = 0x3E;
/// Register-width caps used by set_video_timing.
pub const max_sync: u16 = 0x0FFF;
pub const max_porch: u16 = 0x1FFF;
pub const max_active: u16 = 0x7FFF;

/// Mirrors `ra8_mipi_dsi_video_cfg_t` (26 B, 1-byte enums).
pub const VideoCfg = extern struct {
    pixel_format: u8,
    virtual_channel: u8,
    sync_pulse: bool,
    hsa_no_lp: bool,
    hbp_no_lp: bool,
    hfp_no_lp: bool,
    vsync_active_high: bool,
    hsync_active_high: bool,
    vertical_sync_lines: u16,
    vertical_active_lines: u16,
    vertical_back_porch: u16,
    vertical_front_porch: u16,
    horizontal_sync_lines: u16,
    horizontal_active_pixels: u16,
    horizontal_back_porch: u16,
    horizontal_front_porch: u16,
    video_mode_delay: u16,
};

/// Mirrors `ra8_mipi_dsi_video_timing_t` (16 B).
pub const VideoTiming = extern struct {
    horizontal_sync: u16,
    horizontal_back_porch: u16,
    horizontal_active: u16,
    horizontal_front_porch: u16,
    vertical_sync: u16,
    vertical_back_porch: u16,
    vertical_active: u16,
    vertical_front_porch: u16,
};

fn flag(set: bool, bit: u32) u32 {
    return if (set) bit else 0;
}

/// Low field in bits [n:0], high field shifted to 16, optional polarity.
fn pair(lo: u16, lo_mask: u32, hi: u16, hi_mask: u32) u32 {
    return (@as(u32, lo) & lo_mask) | ((@as(u32, hi) << 16) & hi_mask);
}

pub fn vmset1Word(c: *const VideoCfg) u32 {
    return (@as(u32, c.video_mode_delay) << 2) & vmset1_dly_mask;
}

pub fn vmppWord(c: *const VideoCfg) u32 {
    const dt = (@as(u32, c.pixel_format) << 16) & vmpp_dt_mask;
    const vc = (@as(u32, c.virtual_channel) << 22) & vmpp_vc_mask;
    return dt | vc | flag(c.sync_pulse, vmpp_txesync);
}

pub fn vmvsWord(c: *const VideoCfg) u32 {
    const w = pair(c.vertical_sync_lines, sync_mask, c.vertical_active_lines, active_hi_mask);
    return w | flag(c.vsync_active_high, pol_bit);
}

pub fn vmvpWord(c: *const VideoCfg) u32 {
    return pair(c.vertical_back_porch, porch_mask, c.vertical_front_porch, porch_hi_mask);
}

pub fn vmhsWord(c: *const VideoCfg) u32 {
    const w = pair(c.horizontal_sync_lines, sync_mask, c.horizontal_active_pixels, active_hi_mask);
    return w | flag(c.hsync_active_high, pol_bit);
}

pub fn vmhpWord(c: *const VideoCfg) u32 {
    return pair(c.horizontal_back_porch, porch_mask, c.horizontal_front_porch, porch_hi_mask);
}

pub fn vmset0StartWord(c: *const VideoCfg) u32 {
    return vmset0_vstart | flag(c.hsa_no_lp, vmset0_hsanolp) |
        flag(c.hbp_no_lp, vmset0_hbpnolp) | flag(c.hfp_no_lp, vmset0_hfpnolp);
}

pub fn configure(dsi: anytype, cfg: ?*const VideoCfg) u16 {
    const c = cfg orelse {
        dsi.err("vcfg must not be nullptr");
        return null_ptr;
    };
    if (c.virtual_channel > vc_max) return invalid_arg;
    dsi.write32(off_vmset1r, vmset1Word(c));
    dsi.write32(off_vmppsetr, vmppWord(c));
    dsi.write32(off_vmvssetr, vmvsWord(c));
    dsi.write32(off_vmvpsetr, vmvpWord(c));
    dsi.write32(off_vmhssetr, vmhsWord(c));
    dsi.write32(off_vmhpsetr, vmhpWord(c));
    return ok;
}

pub fn start(dsi: anytype, cfg: ?*const VideoCfg) u16 {
    const c = cfg orelse {
        dsi.err("vcfg must not be nullptr");
        return null_ptr;
    };
    dsi.write32(off_vmset0r, vmset0StartWord(c));
    return dsi.waitEq(off_vmsr, vmsr_virdy, vmsr_virdy);
}

pub fn stop(dsi: anytype) u16 {
    dsi.write32(off_vmset0r, vmset0_vstop);
    const rc = dsi.waitEq(off_vmsr, vmsr_stop, vmsr_stop);
    if (rc == ok) dsi.write32(off_vmscr, vmsr_clear_all);
    return rc;
}

pub fn timingInRange(t: *const VideoTiming) bool {
    if (t.horizontal_sync > max_sync or t.vertical_sync > max_sync) return false;
    if (t.horizontal_back_porch > max_porch or t.horizontal_front_porch > max_porch) return false;
    if (t.vertical_back_porch > max_porch or t.vertical_front_porch > max_porch) return false;
    return t.horizontal_active <= max_active and t.vertical_active <= max_active;
}

/// Full config with the driver defaults: RGB888 on VC0, no sync pulse,
/// blanking kept HS, both syncs active high, no start delay.
pub fn defaultCfg(t: *const VideoTiming) VideoCfg {
    return .{
        .pixel_format = dt_rgb888,
        .virtual_channel = 0,
        .sync_pulse = false,
        .hsa_no_lp = true,
        .hbp_no_lp = true,
        .hfp_no_lp = true,
        .vsync_active_high = true,
        .hsync_active_high = true,
        .vertical_sync_lines = t.vertical_sync,
        .vertical_active_lines = t.vertical_active,
        .vertical_back_porch = t.vertical_back_porch,
        .vertical_front_porch = t.vertical_front_porch,
        .horizontal_sync_lines = t.horizontal_sync,
        .horizontal_active_pixels = t.horizontal_active,
        .horizontal_back_porch = t.horizontal_back_porch,
        .horizontal_front_porch = t.horizontal_front_porch,
        .video_mode_delay = 0,
    };
}

pub fn setVideoTiming(dsi: anytype, timing: ?*const VideoTiming) u16 {
    const t = timing orelse {
        dsi.err("timing must not be nullptr");
        return null_ptr;
    };
    if (!timingInRange(t)) return invalid_arg;
    const v = defaultCfg(t);
    return configure(dsi, &v);
}
