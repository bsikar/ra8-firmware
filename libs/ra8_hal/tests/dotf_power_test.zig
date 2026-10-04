//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Host tests for internal/dotf_power.zig.

const std = @import("std");
const power = @import("dotf_power");

/// Records each primitive call; `fail_at` makes that call number return 0x104.
const Fake = struct {
    calls: [16][]const u8 = undefined,
    count: usize = 0,
    fail_at: ?usize = null,
    logged: [8][*:0]const u8 = undefined,
    logs: usize = 0,
    region: power.Region = undefined,

    fn hit(self: *Fake, name: []const u8) u16 {
        self.calls[self.count] = name;
        self.count += 1;
        if (self.fail_at == self.count) return 0x104;
        return 0;
    }
    pub fn init(self: *Fake) u16 {
        return self.hit("init");
    }
    pub fn installKey(self: *Fake, _: u8, _: *const power.KeyHandle) u16 {
        return self.hit("key");
    }
    pub fn setIv(self: *Fake, _: u8, _: *const [power.iv_word_count]u32) u16 {
        return self.hit("iv");
    }
    pub fn setRegion(self: *Fake, _: u8, r: *const power.Region) u16 {
        self.region = r.*;
        return self.hit("region");
    }
    pub fn selectRegion(self: *Fake, _: u8, _: u8) u16 {
        return self.hit("select");
    }
    pub fn setScaLevel(self: *Fake, _: u8, _: u8) u16 {
        return self.hit("sca");
    }
    pub fn enable(self: *Fake, _: u8) u16 {
        return self.hit("enable");
    }
    pub fn mstpEnable(self: *Fake, _: u16) u16 {
        return self.hit("mstp_on");
    }
    pub fn mstpDisable(self: *Fake, _: u16) u16 {
        return self.hit("mstp_off");
    }
    pub fn fail(self: *Fake, msg: [*:0]const u8, _: u16) void {
        self.logged[self.logs] = msg;
        self.logs += 1;
    }
};

fn cfgFor(channel: u8, enable_after: bool) power.OpenCfg {
    return std.mem.zeroInit(power.OpenCfg, .{ .channel = channel, .enable_after = enable_after });
}

test "struct layouts match the C headers" {
    try std.testing.expectEqual(@as(usize, 12), @sizeOf(power.Region));
    try std.testing.expectEqual(@as(usize, 40), @sizeOf(power.KeyHandle));
    try std.testing.expectEqual(@as(usize, 76), @sizeOf(power.OpenCfg));
    try std.testing.expectEqual(@as(usize, 44), @offsetOf(power.OpenCfg, "iv_words"));
    try std.testing.expectEqual(@as(usize, 60), @offsetOf(power.OpenCfg, "region"));
    try std.testing.expectEqual(@as(usize, 73), @offsetOf(power.OpenCfg, "enable_after"));
}

test "open runs every step in order and enables when asked" {
    var f = Fake{};
    const cfg = cfgFor(1, true);
    try std.testing.expectEqual(@as(u16, 0), power.open(&f, &cfg));
    const want = [_][]const u8{ "init", "key", "iv", "region", "select", "sca", "enable" };
    try std.testing.expectEqual(want.len, f.count);
    for (want, 0..) |name, i| try std.testing.expectEqualStrings(name, f.calls[i]);
    try std.testing.expectEqual(@as(usize, 0), f.logs);
}

test "open skips enable when enable_after is false" {
    var f = Fake{};
    const cfg = cfgFor(0, false);
    try std.testing.expectEqual(@as(u16, 0), power.open(&f, &cfg));
    try std.testing.expectEqual(@as(usize, 6), f.count);
}

test "open rejects a bad channel before touching hardware" {
    var f = Fake{};
    const cfg = cfgFor(2, true);
    try std.testing.expectEqual(@as(u16, 0x103), power.open(&f, &cfg));
    try std.testing.expectEqual(@as(usize, 0), f.count);
    try std.testing.expectEqualStrings("open: validate_init", std.mem.span(f.logged[0]));
}

test "open stops at a failing step and logs inner then outer" {
    var f = Fake{ .fail_at = 3 };
    const cfg = cfgFor(0, true);
    try std.testing.expectEqual(@as(u16, 0x104), power.open(&f, &cfg));
    try std.testing.expectEqual(@as(usize, 3), f.count);
    try std.testing.expectEqual(@as(usize, 2), f.logs);
    try std.testing.expectEqualStrings("open: set_iv", std.mem.span(f.logged[0]));
    try std.testing.expectEqualStrings("open: stage", std.mem.span(f.logged[1]));
}

test "window needs a non-empty 4 KiB-aligned span and ends inclusive" {
    try std.testing.expectEqual(@as(?power.Region, null), power.window(0x1000, 0));
    try std.testing.expectEqual(@as(?power.Region, null), power.window(0x1001, 0x1000));
    try std.testing.expectEqual(@as(?power.Region, null), power.window(0x1000, 0x0800));
    const r = power.window(0x8000_0000, 0x2000).?;
    try std.testing.expectEqual(@as(u32, 0x8000_1FFF), r.end_addr);
    try std.testing.expectEqual(@as(u32, 0x0000_0FFF), power.window(0xFFFF_F000, 0x2000).?.end_addr);
}

test "setRegionWindow checks the channel and stages region 0" {
    var f = Fake{};
    try std.testing.expectEqual(@as(u16, 0x103), power.setRegionWindow(&f, 2, 0x1000, 0x1000));
    try std.testing.expectEqual(@as(u16, 0x103), power.setRegionWindow(&f, 0, 0x1000, 0));
    try std.testing.expectEqual(@as(usize, 0), f.count);
    try std.testing.expectEqual(@as(u16, 0), power.setRegionWindow(&f, 1, 0x3000, 0x1000));
    try std.testing.expectEqual(power.Region{ .start_addr = 0x3000, .end_addr = 0x3FFF, .key_index = 0, .region_id = 0 }, f.region);
}

test "enterStop gates both channels and ignores errors" {
    var f = Fake{ .fail_at = 1 };
    try std.testing.expectEqual(@as(u16, 0), power.enterStop(&f));
    try std.testing.expectEqual(@as(usize, 2), f.count);
    try std.testing.expectEqual(@as(u16, 0x110), power.mstp_ids[0]);
    try std.testing.expectEqual(@as(u16, 0x111), power.mstp_ids[1]);
}

test "exitStop stops at the first enable error" {
    var ok_fake = Fake{};
    try std.testing.expectEqual(@as(u16, 0), power.exitStop(&ok_fake));
    try std.testing.expectEqual(@as(usize, 2), ok_fake.count);
    var f = Fake{ .fail_at = 1 };
    try std.testing.expectEqual(@as(u16, 0x104), power.exitStop(&f));
    try std.testing.expectEqual(@as(usize, 1), f.count);
    try std.testing.expectEqualStrings("exit_stop: mstp enable failed", std.mem.span(f.logged[0]));
}
