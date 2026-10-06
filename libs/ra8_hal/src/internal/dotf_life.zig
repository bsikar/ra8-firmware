//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF init/deinit (RA8FW-838, part of ra8_dotf.c). Pure over an `ops`
//! value; the exports and the register binding are in src/dotf_life_abi.zig.

const power = @import("dotf_power.zig");

/// Per channel: gate the clock on (HUM 45.6.1), zero CONVAREAST/CONVAREAD
/// and REG00 (HUM 45.3), reset the software state. Then drop the handler.
/// An MSTP failure is logged and returned before later channels are touched.
pub fn init(ops: anytype) u16 {
    var ch: u8 = 0;
    while (ch < power.channel_count) : (ch += 1) {
        const err = ops.mstpEnable(power.mstp_ids[ch]);
        if (err != 0) {
            ops.fail("dotf_init: mstp enable failed", err);
            return err;
        }
        ops.channelReset(ch);
        ops.stateReset(ch);
    }
    ops.clearHandler();
    ops.info("dotf_init");
    return 0;
}

/// Per channel: force bypass (REG00 = 0), reset the state, gate the clock
/// off ignoring its result. Then drop the handler.
pub fn deinit(ops: anytype) void {
    var ch: u8 = 0;
    while (ch < power.channel_count) : (ch += 1) {
        ops.disable(ch);
        ops.stateReset(ch);
        _ = ops.mstpDisable(power.mstp_ids[ch]);
    }
    ops.clearHandler();
}
