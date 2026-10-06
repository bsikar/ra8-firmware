//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF open/close orchestration, region window and module stop
//! (RA8FW-581, was ra8_dotf_power.c). Pure: the DOTF primitives,
//! MSTP and logging come in through an `ops` value. HUM Ch 45.

pub const channel_count: u8 = 2;
/// Low 12 bits of CONVAREAST/CONVAREAD read as 0/1 (HUM 45.3.1/45.3.2).
pub const addr_low_mask: u32 = 0x0000_0FFF;
pub const iv_word_count = 4;
/// MSTPB16 (OSPI0+DOTF0), MSTPB17 (OSPI1+DOTF1): (reg_b << 8) | bit.
pub const mstp_ids = [channel_count]u16{ (1 << 8) | 16, (1 << 8) | 17 };

const ok: u16 = 0;
const invalid_arg: u16 = 0x103;

/// `ra8_dotf_region_t` (inc/ra8_dotf.h).
pub const Region = extern struct {
    start_addr: u32,
    end_addr: u32,
    key_index: u8,
    region_id: u8,
};

/// `ra8_dotf_key_handle_t` (inc/ra8_dotf.h).
pub const KeyHandle = extern struct {
    size: u32,
    key_index: u8,
    valid: u8,
    words: [8]u32,
};

/// `ra8_dotf_open_cfg_t` (inc/ra8_dotf.h).
pub const OpenCfg = extern struct {
    channel: u8,
    key: KeyHandle,
    iv_words: [iv_word_count]u32,
    region: Region,
    sca_level: u8,
    enable_after: bool,
};

pub fn channelInRange(channel: u8) bool {
    return channel < channel_count;
}

/// The 4 KiB-aligned window [start, start + len - 1], or null when it is
/// empty or unaligned. The end wraps like the C uint32 sum did.
pub fn window(start: u32, len: u32) ?Region {
    if (len == 0) return null;
    if (start & addr_low_mask != 0 or len & addr_low_mask != 0) return null;
    return .{ .start_addr = start, .end_addr = start +% len -% 1, .key_index = 0, .region_id = 0 };
}

/// RA8_RETURN_ON_ERROR: on error, log `msg` (then the code) and hand it up.
fn check(ops: anytype, err: u16, msg: [*:0]const u8) ?u16 {
    if (err == ok) return null;
    ops.fail(msg, err);
    return err;
}

/// ra8_dotf_open: validate + init, stage key/IV/region, SCA level, enable.
pub fn open(ops: anytype, cfg: *const OpenCfg) u16 {
    if (check(ops, validateInit(ops, cfg), "open: validate_init")) |e| return e;
    if (check(ops, stage(ops, cfg), "open: stage")) |e| return e;
    if (check(ops, finalise(ops, cfg), "open: finalise")) |e| return e;
    return ok;
}

fn validateInit(ops: anytype, cfg: *const OpenCfg) u16 {
    if (!channelInRange(cfg.channel)) return invalid_arg;
    // HUM Ch 45.6.1 p 3050: power on the block (idempotent).
    if (check(ops, ops.init(), "open: init")) |e| return e;
    return ok;
}

fn stage(ops: anytype, cfg: *const OpenCfg) u16 {
    const ch = cfg.channel;
    if (check(ops, ops.installKey(ch, &cfg.key), "open: install_key")) |e| return e;
    if (check(ops, ops.setIv(ch, &cfg.iv_words), "open: set_iv")) |e| return e;
    if (check(ops, ops.setRegion(ch, &cfg.region), "open: set_region")) |e| return e;
    if (check(ops, ops.selectRegion(ch, cfg.region.region_id), "open: select_region")) |e| return e;
    return ok;
}

fn finalise(ops: anytype, cfg: *const OpenCfg) u16 {
    if (check(ops, ops.setScaLevel(cfg.channel, cfg.sca_level), "open: set_sca_level")) |e| return e;
    if (cfg.enable_after) {
        if (check(ops, ops.enable(cfg.channel), "open: enable")) |e| return e;
    }
    return ok;
}

/// ra8_dotf_set_region_window: stage a 4 KiB-aligned window as region 0.
pub fn setRegionWindow(ops: anytype, channel: u8, start: u32, len: u32) u16 {
    if (!channelInRange(channel)) return invalid_arg;
    const r = window(start, len) orelse return invalid_arg;
    return ops.setRegion(channel, &r);
}

/// ra8_dotf_enter_stop: gate both channels; disable errors are ignored.
pub fn enterStop(ops: anytype) u16 {
    for (mstp_ids) |id| _ = ops.mstpDisable(id);
    return ok;
}

/// ra8_dotf_exit_stop: ungate both channels, stop at the first error.
pub fn exitStop(ops: anytype) u16 {
    for (mstp_ids) |id| {
        if (check(ops, ops.mstpEnable(id), "exit_stop: mstp enable failed")) |e| return e;
    }
    return ok;
}
