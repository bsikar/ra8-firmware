//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DFU_GETSTATUS and the bounded wait built on it. The driver reads exactly
//! one field of the six-byte payload, `bState`.

const Err = @import("err").Err;
const hal = @import("hal");
const proto = @import("proto");
const tune = @import("tune");

/// One DFU_GETSTATUS. A short payload is a hardware error, not a partial read.
pub fn getState(comptime H: type, speed: hal.Speed, out_state: *u8) Err {
    var payload: [proto.GetStatus.len]u8 = @splat(0);
    const setup = hal.Setup{
        .bm_request_type = proto.Bm.class_if_in,
        .b_request = proto.Request.dfu_getstatus,
        .w_value = 0,
        .w_index = proto.Session.interface,
        .w_length = proto.GetStatus.len,
    };
    var received: u16 = 0;
    const err = H.controlXfer(speed, &setup, &payload, proto.GetStatus.len, &received);
    if (!err.isOk()) return err;
    if (received != proto.GetStatus.len) return .hw_error;
    out_state.* = payload[proto.GetStatus.state_offset];
    return .ok;
}

/// Poll until the device reports `want`, or give up.
///
/// A failing poll aborts immediately: the device is not going to argue its
/// way into the wanted state, and retrying a broken control pipe only
/// delays the caller's error.
pub fn waitFor(comptime H: type, speed: hal.Speed, want: u8) Err {
    var polls: u32 = 0;
    while (polls < tune.Retry.status_tries) : (polls += 1) {
        var state: u8 = 0;
        const err = getState(H, speed, &state);
        if (!err.isOk()) return err;
        if (state == want) return .ok;
        H.delayMs(tune.Delay.status_poll_ms);
    }
    return .hw_timeout;
}
