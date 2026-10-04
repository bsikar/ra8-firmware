//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the I3C legacy-I2C responder
//! (internal/i3c_i2c_peripheral.zig, RA8FW-557). Built as its own object
//! in libra8_hal.a (RA8FW-542). Check order and log lines match the
//! deleted ra8_i3c_i2c_peripheral.c.

const common = @import("abi_common.zig");
const p = @import("internal/i3c_i2c_peripheral.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const k_ra8_err_hw_timeout = common.k_ra8_err_hw_timeout;

/// `k_ra8_mstp_i3c`: (k_ra8_mstp_reg_b << 8) | 4 (inc/ra8_mstp_regs.h).
const mstp_i3c: u16 = (1 << 8) | 4;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

const tag = "IICBP";

fn logError(msg: [*:0]const u8) void {
    common.ra8_log_emit_error(tag, msg);
}

/// `RA8_RETURN_ON_ERROR` with hw_timeout.
fn timedOut(msg: [*:0]const u8) u16 {
    logError(msg);
    common.ra8_log_emit_error_val(tag, "Error", k_ra8_err_hw_timeout);
    return k_ra8_err_hw_timeout;
}

/// `ra8_err_t ra8_i3c_i2c_peripheral_open(uint8_t, const ra8_i3c_i2c_peripheral_cfg_t*)`.
export fn ra8_i3c_i2c_peripheral_open(channel: u8, cfg: ?*const p.Cfg) u16 {
    const c = cfg orelse {
        logError("ra8_i3c_i2c_peripheral_open: cfg null");
        return k_ra8_err_null_ptr;
    };
    const block = p.regsFor(channel) orelse {
        logError("ra8_i3c_i2c_peripheral_open: channel out of range");
        return k_ra8_err_invalid_arg;
    };
    if (c.peripheral_addr_7b > p.max_addr_7b) return k_ra8_err_invalid_arg;
    // HUM Ch 11.2.7 MSTPCRB: the block is gated at reset, so ungate MSTPB4
    // before the first register write and touch nothing if that fails.
    const mst_err = ra8_mstp_enable(mstp_i3c);
    if (mst_err == k_ra8_ok) {
        p.configure(block, c.*);
        common.ra8_log_emit_info_val(tag, "ra8_i3c_i2c_peripheral_open ch", channel);
    }
    return mst_err;
}

/// `ra8_err_t ra8_i3c_i2c_peripheral_close(uint8_t channel)`.
export fn ra8_i3c_i2c_peripheral_close(channel: u8) u16 {
    const block = p.regsFor(channel) orelse return k_ra8_err_invalid_arg;
    p.clear(block);
    return ra8_mstp_disable(mstp_i3c);
}

/// `ra8_err_t ra8_i3c_i2c_peripheral_send(uint8_t, const uint8_t*, uint32_t)`.
export fn ra8_i3c_i2c_peripheral_send(channel: u8, data: ?[*]const u8, len: u32) u16 {
    const block = p.regsFor(channel) orelse return k_ra8_err_invalid_arg;
    if (len == 0) return k_ra8_ok;
    const bytes = data orelse {
        logError("ra8_i3c_i2c_peripheral_send: data null");
        return k_ra8_err_null_ptr;
    };
    p.send(block, bytes[0..len], p.spin_budget) catch
        return timedOut("ra8_i3c_i2c_peripheral_send: TDBEF0 wait");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_i3c_i2c_peripheral_receive(uint8_t, uint8_t*, uint32_t)`.
export fn ra8_i3c_i2c_peripheral_receive(channel: u8, buf: ?[*]u8, len: u32) u16 {
    const block = p.regsFor(channel) orelse return k_ra8_err_invalid_arg;
    if (len == 0) return k_ra8_ok;
    const bytes = buf orelse {
        logError("ra8_i3c_i2c_peripheral_receive: buf null");
        return k_ra8_err_null_ptr;
    };
    p.receive(block, bytes[0..len], p.spin_budget) catch
        return timedOut("ra8_i3c_i2c_peripheral_receive: RDBFF0 wait");
    return k_ra8_ok;
}

/// `ra8_err_t ra8_i3c_i2c_peripheral_status(uint8_t channel, uint8_t* out_mask)`.
export fn ra8_i3c_i2c_peripheral_status(channel: u8, out_mask: ?*u8) u16 {
    const out = out_mask orelse {
        logError("ra8_i3c_i2c_peripheral_status: out_mask null");
        return k_ra8_err_null_ptr;
    };
    const block = p.regsFor(channel) orelse return k_ra8_err_invalid_arg;
    out.* = p.statusMask(block);
    return k_ra8_ok;
}
