//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! I3C legacy-I2C controller bus probe (RA8FW-693): START, one address
//! byte with R/W = write, then wait for TENDF (ACK) or NACKDF. Generic
//! over the bus helpers so host tests can record the sequence.

/// HUM 40.2 BST.NACKDF, p 2482.
pub const bst_nackdf: u32 = 1 << 4;
/// HUM 40.2 BST.TENDF, p 2482.
pub const bst_tendf: u32 = 1 << 8;
/// Status-flag spin budget (`k_ra8_i3c_i2c_ctrl_poll_limit`).
pub const poll_limit: u32 = 200000;

const ok: u16 = 0;
const hw_timeout: u16 = 0x203;

/// On-the-wire byte for a 7-bit target with R/W = write.
pub fn addressByte(target_7b: u8) u8 {
    return @truncate(@as(u32, target_7b) << 1);
}

/// `bus` provides clearBst, start, stop and sendAddress(u8) u16, the
/// promoted `priv_i3c_i2c_*` helpers. Stop is best-effort on every exit.
pub fn run(bus: anytype, bst: *const volatile u32, target_7b: u8, out_acked: *bool) u16 {
    out_acked.* = false;
    bus.clearBst();
    bus.start();
    const sent = bus.sendAddress(addressByte(target_7b));
    if (sent != ok) {
        bus.stop();
        return sent;
    }
    var err: u16 = hw_timeout;
    var i: u32 = 0;
    while (i < poll_limit) : (i += 1) {
        const v = bst.*;
        if ((v & (bst_tendf | bst_nackdf)) != 0) {
            out_acked.* = (v & bst_nackdf) == 0;
            err = ok;
            break;
        }
    }
    bus.stop();
    bus.clearBst();
    return err;
}
