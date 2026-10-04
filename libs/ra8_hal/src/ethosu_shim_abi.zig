//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the Arm ethos-u-core-driver adapter over ra8_npu
//! (internal/ethosu_shim.zig, RA8FW-573). Built as its own object in
//! libra8_hal.a, so only an image that calls ethosu_* (the RA8P1 TFLite
//! Ethos-U operator) pulls it in; ra8_npu itself is still C.

const common = @import("abi_common.zig");
const shim = @import("internal/ethosu_shim.zig");

const tag = "ETHOSU";
const ret_ok: c_int = 0;
const ret_err: c_int = -1;

extern fn ra8_npu_init() u16;
extern fn ra8_npu_submit(job: *const shim.Job) u16;
extern fn ra8_npu_run() u16;
extern fn ra8_npu_wait() u16;

/// The single persistent NPU context every reserve hands out.
var driver: shim.Driver = .{};
var ready: bool = false;

/// `struct ethosu_driver* ethosu_reserve_driver(void)`.
export fn ethosu_reserve_driver() ?*shim.Driver {
    if (!ready) {
        if (ra8_npu_init() != common.k_ra8_ok) {
            common.ra8_log_emit_error(tag, "reserve: npu_init failed");
            return null;
        }
        ready = true;
    }
    driver.reserved = true;
    return &driver;
}

/// `int ethosu_invoke_v3(...)`. base_addr_size and user_arg have no
/// analogue in the ra8_npu job model.
export fn ethosu_invoke_v3(
    drv: ?*shim.Driver,
    custom_data_ptr: ?*const anyopaque,
    custom_data_size: c_int,
    base_addr: ?[*]const u64,
    base_addr_size: ?[*]const usize,
    num_base_addr: c_int,
    user_arg: ?*anyopaque,
) c_int {
    _ = base_addr_size;
    _ = user_arg;
    const args = shim.Invoke{
        .drv = drv,
        .cmd = custom_data_ptr,
        .size = custom_data_size,
        .bases = base_addr,
        .count = num_base_addr,
    };
    shim.check(args) catch |e| {
        common.ra8_log_emit_error(tag, shim.message(e));
        return ret_err;
    };
    const job = shim.buildJob(args);
    if (ra8_npu_submit(&job) != common.k_ra8_ok) return fail("invoke: submit");
    if (ra8_npu_run() != common.k_ra8_ok) return fail("invoke: run");
    if (ra8_npu_wait() != common.k_ra8_ok) return fail("invoke: wait");
    return ret_ok;
}

/// `void ethosu_release_driver(struct ethosu_driver*)`. The NPU stays
/// initialised so the next reserve re-hands the same context.
export fn ethosu_release_driver(drv: ?*shim.Driver) void {
    const d = drv orelse {
        common.ra8_log_emit_error(tag, "release: null driver");
        return;
    };
    d.reserved = false;
}

fn fail(msg: [*:0]const u8) c_int {
    common.ra8_log_emit_error(tag, msg);
    return ret_err;
}
