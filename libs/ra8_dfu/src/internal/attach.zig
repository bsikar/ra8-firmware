//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Getting from a powered port to a device that answers: wait for the D+
//! pull-up, then reset-and-probe until the DEVICE descriptor comes back.

const Err = @import("err").Err;
const control = @import("control");
const hal = @import("hal");
const proto = @import("proto");
const tune = @import("tune");

/// Spin until the line goes high, bounded twice over: by the millisecond
/// clock and by an iteration cap, so neither a dead port nor a stopped clock
/// can wedge the caller.
fn awaitLine(comptime H: type, speed: hal.Speed) void {
    H.delayMs(tune.Delay.vbus_settle_ms);
    const started = H.timeMs();
    var spin: u32 = 0;
    while (spin < tune.Retry.attach_spin) : (spin += 1) {
        if (H.lineState(speed) != 0) break;
        if ((H.timeMs() -% started) > tune.Delay.attach_timeout_ms) break;
    }
    H.delayMs(tune.Delay.debounce_ms);
}

/// Wait for attach, then reset and probe up to `enum_tries` times.
///
/// The reset steps are issued for effect: a controller that refuses one is
/// not worth distinguishing from a device that does not answer, because the
/// descriptor read is the only evidence that matters and it is retried
/// anyway. The error returned is the last probe's.
pub fn hunt(
    comptime H: type,
    speed: hal.Speed,
    desc: *[proto.DeviceDescriptor.len]u8,
) Err {
    awaitLine(H, speed);
    var err: Err = .hw_timeout;
    var attempt: u8 = 0;
    while (attempt < tune.Retry.enum_tries) : (attempt += 1) {
        _ = H.busReset(speed, true);
        H.delayMs(tune.Delay.reset_hold_ms);
        _ = H.busReset(speed, false);
        _ = H.setUact(speed, true);
        H.delayMs(tune.Delay.recovery_ms);
        _ = H.setTarget(speed, 0);
        err = control.getDeviceDescriptor(H, speed, desc);
        if (err.isOk()) return .ok;
    }
    return err;
}
