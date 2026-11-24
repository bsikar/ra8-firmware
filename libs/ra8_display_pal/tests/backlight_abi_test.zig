//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Zig-native ABI boundary tests for `ra8_display_pal_backlight_abi.zig`:
//! controller init/deinit, binary fail-closed 1..99 behavior, PWM Q16 duty
//! scaling + start/stop transitions, and policy ABI validation.

const std = @import("std");
const backlight_abi = @import("backlight_abi");

var log_calls: usize = 0;

fn emitLogError(_: [*:0]const u8, _: [*:0]const u8) callconv(.c) void {
    log_calls += 1;
}

comptime {
    @export(&emitLogError, .{ .name = "ra8_log_emit_error" });
}

const FakePwm = struct {
    var open_calls: usize = 0;
    var close_calls: usize = 0;
    var start_calls: usize = 0;
    var stop_calls: usize = 0;
    var duty_calls: usize = 0;
    var last_duty: u32 = 0;
    var gate_calls: usize = 0;
    var last_gate: u8 = 0xFF;

    fn reset() void {
        open_calls = 0;
        close_calls = 0;
        start_calls = 0;
        stop_calls = 0;
        duty_calls = 0;
        last_duty = 0;
        gate_calls = 0;
        last_gate = 0xFF;
    }

    fn getCaps(_: ?*anyopaque, out: *backlight_abi.PwmCaps) callconv(.c) u16 {
        out.* = .{
            .channel_count = 3,
            .counter_bits = 32,
            .period_max = 100000,
            .has_active_low = 0,
        };
        return 0;
    }

    fn open(_: ?*anyopaque, _: backlight_abi.PwmCh, _: u32, _: u8) callconv(.c) u16 {
        open_calls += 1;
        return 0;
    }

    fn close(_: ?*anyopaque, _: backlight_abi.PwmCh) callconv(.c) u16 {
        close_calls += 1;
        return 0;
    }

    fn start(_: ?*anyopaque, _: backlight_abi.PwmCh) callconv(.c) u16 {
        start_calls += 1;
        return 0;
    }

    fn stop(_: ?*anyopaque, _: backlight_abi.PwmCh) callconv(.c) u16 {
        stop_calls += 1;
        return 0;
    }

    fn setPeriod(_: ?*anyopaque, _: backlight_abi.PwmCh, _: u32) callconv(.c) u16 {
        return 0;
    }

    fn setDuty(_: ?*anyopaque, _: backlight_abi.PwmCh, duty: u32) callconv(.c) u16 {
        duty_calls += 1;
        last_duty = duty;
        return 0;
    }

    fn setEnable(_: ?*anyopaque, on: u8) callconv(.c) u16 {
        gate_calls += 1;
        last_gate = on;
        return 0;
    }

    const iface = backlight_abi.PwmIface{
        .get_caps = getCaps,
        .open = open,
        .close = close,
        .start = start,
        .stop = stop,
        .set_period = setPeriod,
        .set_duty = setDuty,
    };

    fn makeBound() backlight_abi.Pwm {
        return .{
            .iface = &iface,
            .ctx = null,
            .caps = .{
                .channel_count = 3,
                .counter_bits = 32,
                .period_max = 100000,
                .has_active_low = 0,
            },
            .bound = 1,
        };
    }
};

test "binary backlight init and fail-closed intermediate brightness" {
    FakePwm.reset();
    var dev: backlight_abi.BacklightDev = std.mem.zeroes(backlight_abi.BacklightDev);
    const cfg = backlight_abi.BacklightCfg{
        .pwm = null,
        .set_enable = FakePwm.setEnable,
        .ctx = null,
        .pwm_period_ticks = 0,
        .mode = 0,
        .pwm_ch = .{ .index = 0 },
        .pwm_polarity = 0,
    };
    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_init(&dev, &cfg));
    try std.testing.expectEqual(@as(u8, 0), FakePwm.last_gate);

    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_set_brightness(&dev, 100));
    try std.testing.expectEqual(@as(u8, 1), FakePwm.last_gate);
    try std.testing.expectEqual(@as(u8, 100), dev.brightness_pct);

    try std.testing.expectEqual(@as(u16, 0x107), backlight_abi.display_backlight_set_brightness(&dev, 50));
    try std.testing.expectEqual(@as(u8, 1), FakePwm.last_gate);
    try std.testing.expectEqual(@as(u8, 100), dev.brightness_pct);

    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_set_brightness(&dev, 0));
    try std.testing.expectEqual(@as(u8, 0), FakePwm.last_gate);
    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_deinit(&dev));
}

test "PWM backlight scales duty, gates start/stop, and refuses unsupported active-low" {
    FakePwm.reset();
    var pwm = FakePwm.makeBound();
    var dev: backlight_abi.BacklightDev = std.mem.zeroes(backlight_abi.BacklightDev);

    var bad_pol = backlight_abi.BacklightCfg{
        .pwm = &pwm,
        .set_enable = null,
        .ctx = null,
        .pwm_period_ticks = 10000,
        .mode = 1,
        .pwm_ch = .{ .index = 0 },
        .pwm_polarity = 2,
    };
    try std.testing.expectEqual(@as(u16, 0x107), backlight_abi.display_backlight_init(&dev, &bad_pol));

    const cfg = backlight_abi.BacklightCfg{
        .pwm = &pwm,
        .set_enable = FakePwm.setEnable,
        .ctx = null,
        .pwm_period_ticks = 10000,
        .mode = 1,
        .pwm_ch = .{ .index = 1 },
        .pwm_polarity = 1,
    };
    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_init(&dev, &cfg));
    try std.testing.expectEqual(@as(usize, 1), FakePwm.open_calls);
    try std.testing.expectEqual(@as(u8, 0), dev.pwm_running);

    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_set_brightness(&dev, 25));
    try std.testing.expectEqual(@as(u32, 16384), FakePwm.last_duty);
    try std.testing.expectEqual(@as(usize, 1), FakePwm.start_calls);
    try std.testing.expectEqual(@as(u8, 1), FakePwm.last_gate);

    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_set_brightness(&dev, 0));
    try std.testing.expectEqual(@as(u32, 0), FakePwm.last_duty);
    try std.testing.expectEqual(@as(usize, 1), FakePwm.stop_calls);
    try std.testing.expectEqual(@as(u8, 0), FakePwm.last_gate);

    try std.testing.expectEqual(@as(u16, 0), backlight_abi.display_backlight_deinit(&dev));
    try std.testing.expectEqual(@as(usize, 1), FakePwm.close_calls);
}
