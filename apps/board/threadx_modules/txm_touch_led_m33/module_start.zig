//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_touch_led_m33 (RA8FW-1262): a legible ThreadX Module app. It declares
//! `display | led` capabilities in its signed `.ra8app` manifest, clears LED0
//! and draws a dark slate region, polls `touchEvent()` for a screen press, and
//! lights LED0 while repainting the region green, then exits cleanly (`exit(0)`).
//!
//! Every kernel boundary is crossed through `ra8_app_api_t` shims
//! (`libs/ra8_app/src/internal/api/module.zig`): no hardware or kernel
//! pointer is visible inside the module.

const api = @import("ra8_app_api");
const Api = api.Api;
const abi = api.abi;

const calls = Api(api.target);

const Step = enum(u32) {
    version = 1,
    screen_info = 2,
    draw_rect = 3,
    touch_wait = 4,
    led_set = 5,
};

const idle_argb: u32 = 0xFF1F2937;
const active_argb: u32 = 0xFF2ECC71;
const poll_sleep_ms: u32 = 10;

fn run() u32 {
    if (calls.apiVersion() != abi.version) return @backingInt(Step.version);

    const screen = calls.screenInfo() catch return @backingInt(Step.screen_info);
    if (screen.width == 0 or screen.height == 0 or screen.bytes_per_pixel != 4) {
        return @backingInt(Step.screen_info);
    }

    calls.ledSet(0, false) catch return @backingInt(Step.led_set);
    calls.drawRect(.{ .w = screen.width, .h = screen.height }, idle_argb) catch {
        return @backingInt(Step.draw_rect);
    };

    while (true) {
        const maybe = calls.touchEvent() catch return @backingInt(Step.touch_wait);
        if (maybe) |event| {
            if (event.kind == .press) {
                calls.ledSet(0, true) catch return @backingInt(Step.led_set);
                calls.drawRect(.{ .w = screen.width, .h = screen.height }, active_argb) catch {
                    return @backingInt(Step.draw_rect);
                };
                return 0;
            }
        }
        calls.sleep(poll_sleep_ms) catch return @backingInt(Step.touch_wait);
    }
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    const code = run();
    calls.exit(code) catch {};
    while (true) asm volatile ("wfi");
}
