//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! txm_tour_m33's start thread (RA8FW-1023, under RA8FW-289): the tour of
//! `ra8_app_api_t`, the call table a loaded module uses. It drives every
//! request the table defines through the module's typed shims
//! (`libs/ra8_app/src/internal/api/module.zig`): the API generation, the
//! tick clock, a sleep, the screen region, a rect and an image drawn into
//! it, a file written under the module's own directory and read back, the
//! empty touch queue, then `exit(0)`. A step whose answer does not match
//! what the resident image should serve stops the tour with that step's
//! number as the exit code, so the host names the failure.
//!
//! The module never touches a kernel pointer: every call is a ThreadX
//! application request through the shims. Addresses it hands the resident
//! image all come from its data region, so the strings the storage steps
//! pass live in data statics, not the read-only code image. The manifest
//! identity (app id, capabilities) lives in the `.ra8app` the build packs;
//! the resident image admits it before the tour starts.

const std = @import("std");
const api = @import("ra8_app_api");
const Api = api.Api;
const abi = api.abi;

/// The module's own request entry point.
const calls = Api(api.target);

/// The steps, in tour order. A failure exits with the failing step's
/// number, and the host's log names it.
const Step = enum(u32) {
    version = 1,
    ticks = 2,
    sleep = 3,
    screen_info = 4,
    draw_rect = 5,
    draw_image = 6,
    file_write = 7,
    file_read = 8,
    touch_empty = 9,

    /// The last step. One past it is `exit(0)`.
    pub const last: Step = .touch_empty;
};

var pixels: [8 * 8 * 4]u8 = undefined;

/// The resident image reads pointer arguments only from the module's data
/// region, and a string literal compiles into the read-only code image, so
/// the storage steps' strings live here. Initialized statics land in
/// `.data`, which the manager loads into the region it checks against.
var tour_path: ["tour.txt".len]u8 = "tour.txt".*;
var tour_text: ["hello from txm_tour_m33".len]u8 = "hello from txm_tour_m33".*;

fn run() u32 {
    // The generation the resident image serves.
    if (calls.apiVersion() != abi.version) return @backingInt(Step.version);

    // The tick clock runs: the image answers twice, and the tour sleeps
    // between the two so the second call is not the same instant.
    const before = calls.ticks() catch return @backingInt(Step.ticks);
    calls.sleep(2) catch return @backingInt(Step.sleep);
    const after = calls.ticks() catch return @backingInt(Step.ticks);
    if (after < before) return @backingInt(Step.ticks);

    // The region is the host's fixture: 64x64, four bytes a pixel.
    const screen = calls.screenInfo() catch return @backingInt(Step.screen_info);
    if (screen.width != 64 or screen.height != 64 or screen.bytes_per_pixel != 4) {
        return @backingInt(Step.screen_info);
    }

    // A rect: one fill over the whole region.
    calls.drawRect(.{ .w = screen.width, .h = screen.height }, 0x3366CCFF) catch {
        return @backingInt(Step.draw_rect);
    };

    // An image: the top-left 8x8 of the region, from module memory.
    for (&pixels, 0..) |*byte, index| {
        byte.* = @truncate(@as(u32, @intCast(index)) *% 37 +% 11);
    }
    calls.drawImage(.{ .w = 8, .h = 8 }, &pixels) catch {
        return @backingInt(Step.draw_image);
    };

    // Storage: write a file under the module's own directory, close it,
    // open it again, read the bytes back.
    const text: []const u8 = &tour_text;
    const written = calls.fileOpen(&tour_path, abi.OpenFlag.read | abi.OpenFlag.write | abi.OpenFlag.create) catch {
        return @backingInt(Step.file_write);
    };
    const put = calls.fileWrite(written, text) catch return @backingInt(Step.file_write);
    if (put != text.len) return @backingInt(Step.file_write);
    calls.fileClose(written) catch return @backingInt(Step.file_write);

    const opened = calls.fileOpen(&tour_path, abi.OpenFlag.read) catch {
        return @backingInt(Step.file_read);
    };
    var buffer: [text.len]u8 = undefined;
    const got = calls.fileRead(opened, &buffer) catch return @backingInt(Step.file_read);
    if (got != text.len or !std.mem.eql(u8, &buffer, text)) return @backingInt(Step.file_read);
    calls.fileClose(opened) catch return @backingInt(Step.file_read);

    // No input is queued: the image serves an empty touch queue.
    if ((calls.touchEvent() catch return @backingInt(Step.touch_empty)) != null) {
        return @backingInt(Step.touch_empty);
    }

    return 0;
}

export fn demo_module_start(id: u32) callconv(.c) noreturn {
    _ = id;
    const code = run();
    calls.exit(code) catch {};
    // The host stops and unloads the module on the exit request; until
    // then the start thread idles.
    while (true) asm volatile ("wfi");
}
