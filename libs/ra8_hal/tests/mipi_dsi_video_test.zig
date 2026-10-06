//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie

const std = @import("std");
const vd = @import("mipi_dsi_video");

const expect = std.testing.expect;
const expectEqual = std.testing.expectEqual;
const hw_timeout: u16 = 0x203;

const Fake = struct {
    regs: [0x440 / 4]u32 = @splat(0),
    errs: u8 = 0,
    waits: u8 = 0,
    wait_mask: u32 = 0,

    pub fn write32(f: *Fake, off: u16, value: u32) void {
        f.regs[off / 4] = value;
    }
    pub fn waitEq(f: *Fake, off: u16, mask: u32, want: u32) u16 {
        f.waits += 1;
        f.wait_mask = mask;
        return if (f.regs[off / 4] & mask == want) vd.ok else hw_timeout;
    }
    pub fn err(f: *Fake, _: [*:0]const u8) void {
        f.errs += 1;
    }
    fn reg(f: *const Fake, off: u16) u32 {
        return f.regs[off / 4];
    }
};

fn sampleCfg() vd.VideoCfg {
    return .{
        .pixel_format = vd.dt_rgb888,
        .virtual_channel = 1,
        .sync_pulse = true,
        .hsa_no_lp = true,
        .hbp_no_lp = false,
        .hfp_no_lp = true,
        .vsync_active_high = true,
        .hsync_active_high = false,
        .vertical_sync_lines = 4,
        .vertical_active_lines = 600,
        .vertical_back_porch = 8,
        .vertical_front_porch = 6,
        .horizontal_sync_lines = 12,
        .horizontal_active_pixels = 1024,
        .horizontal_back_porch = 64,
        .horizontal_front_porch = 32,
        .video_mode_delay = 5,
    };
}

const sample_timing = vd.VideoTiming{
    .horizontal_sync = 12,
    .horizontal_back_porch = 64,
    .horizontal_active = 1024,
    .horizontal_front_porch = 32,
    .vertical_sync = 4,
    .vertical_back_porch = 8,
    .vertical_active = 600,
    .vertical_front_porch = 6,
};

test "struct layouts match the C header" {
    try expectEqual(@as(usize, 26), @sizeOf(vd.VideoCfg));
    try expectEqual(@as(usize, 8), @offsetOf(vd.VideoCfg, "vertical_sync_lines"));
    try expectEqual(@as(usize, 16), @sizeOf(vd.VideoTiming));
}

test "configure writes all six video-mode words" {
    var f = Fake{};
    const c = sampleCfg();
    try expectEqual(vd.ok, vd.configure(&f, &c));
    try expectEqual(@as(u32, 0x0000_0014), f.reg(vd.off_vmset1r));
    try expectEqual(@as(u32, 0x007E_8000), f.reg(vd.off_vmppsetr));
    try expectEqual(@as(u32, 0x0258_8004), f.reg(vd.off_vmvssetr));
    try expectEqual(@as(u32, 0x0006_0008), f.reg(vd.off_vmvpsetr));
    try expectEqual(@as(u32, 0x0400_000C), f.reg(vd.off_vmhssetr));
    try expectEqual(@as(u32, 0x0020_0040), f.reg(vd.off_vmhpsetr));
}

test "configure masks oversized fields to register width" {
    var f = Fake{};
    var c = sampleCfg();
    c.horizontal_sync_lines = 0xFFFF;
    c.horizontal_active_pixels = 0xFFFF;
    c.video_mode_delay = 0xFFFF;
    c.vertical_front_porch = 0xFFFF;
    try expectEqual(vd.ok, vd.configure(&f, &c));
    try expectEqual(@as(u32, 0x7FFF_0FFF), f.reg(vd.off_vmhssetr));
    try expectEqual(@as(u32, 0x3FFC), f.reg(vd.off_vmset1r));
    try expectEqual(@as(u32, 0x1FFF_0008), f.reg(vd.off_vmvpsetr));
}

test "configure rejects null and an out-of-range virtual channel" {
    var f = Fake{};
    try expectEqual(vd.null_ptr, vd.configure(&f, null));
    try expectEqual(@as(u8, 1), f.errs);
    var c = sampleCfg();
    c.virtual_channel = 4;
    try expectEqual(vd.invalid_arg, vd.configure(&f, &c));
    try expectEqual(@as(u32, 0), f.reg(vd.off_vmppsetr));
}

test "start sets VSTART with the no-LP bits and waits on VIRDY" {
    var f = Fake{};
    const c = sampleCfg();
    f.regs[vd.off_vmsr / 4] = vd.vmsr_virdy;
    try expectEqual(vd.ok, vd.start(&f, &c));
    const want = vd.vmset0_vstart | vd.vmset0_hsanolp | vd.vmset0_hfpnolp;
    try expectEqual(want, f.reg(vd.off_vmset0r));
    try expectEqual(vd.vmsr_virdy, f.wait_mask);
}

test "start passes a poll timeout through and rejects null" {
    var f = Fake{};
    const c = sampleCfg();
    try expectEqual(hw_timeout, vd.start(&f, &c));
    try expectEqual(vd.null_ptr, vd.start(&f, null));
    try expectEqual(@as(u8, 1), f.waits);
    try expectEqual(@as(u8, 1), f.errs);
}

test "stop requests VSTOP and clears VMSCR only once stopped" {
    var f = Fake{};
    try expectEqual(hw_timeout, vd.stop(&f));
    try expectEqual(vd.vmset0_vstop, f.reg(vd.off_vmset0r));
    try expectEqual(@as(u32, 0), f.reg(vd.off_vmscr));
    f.regs[vd.off_vmsr / 4] = vd.vmsr_stop;
    try expectEqual(vd.ok, vd.stop(&f));
    try expectEqual(vd.vmsr_clear_all, f.reg(vd.off_vmscr));
}

test "set_video_timing applies the RGB888 VC0 defaults" {
    var f = Fake{};
    try expectEqual(vd.ok, vd.setVideoTiming(&f, &sample_timing));
    try expectEqual(@as(u32, 0), f.reg(vd.off_vmset1r));
    try expectEqual(@as(u32, 0x003E_0000), f.reg(vd.off_vmppsetr));
    try expectEqual(@as(u32, 0x0258_8004), f.reg(vd.off_vmvssetr));
    try expectEqual(@as(u32, 0x0400_800C), f.reg(vd.off_vmhssetr));
    try expectEqual(@as(u32, 0x0020_0040), f.reg(vd.off_vmhpsetr));
}

test "set_video_timing range-checks every field and rejects null" {
    var f = Fake{};
    try expectEqual(vd.null_ptr, vd.setVideoTiming(&f, null));
    var t = sample_timing;
    t.vertical_sync = vd.max_sync + 1;
    try expectEqual(vd.invalid_arg, vd.setVideoTiming(&f, &t));
    t = sample_timing;
    t.horizontal_front_porch = vd.max_porch + 1;
    try expectEqual(vd.invalid_arg, vd.setVideoTiming(&f, &t));
    t = sample_timing;
    t.vertical_active = vd.max_active + 1;
    try expectEqual(vd.invalid_arg, vd.setVideoTiming(&f, &t));
    t = .{ .horizontal_sync = vd.max_sync, .horizontal_back_porch = vd.max_porch, .horizontal_active = vd.max_active, .horizontal_front_porch = vd.max_porch, .vertical_sync = vd.max_sync, .vertical_back_porch = vd.max_porch, .vertical_active = vd.max_active, .vertical_front_porch = vd.max_porch };
    try expectEqual(vd.ok, vd.setVideoTiming(&f, &t));
    try expect(f.reg(vd.off_vmppsetr) != 0);
}
