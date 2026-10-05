//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_usb_pmsc.h (RA8FW-756), replacing ra8_usb_pmsc.c. The BOT
//! logic is in internal/usb_pmsc.zig; the USB controller calls stay C.

const std = @import("std");
const common = @import("abi_common.zig");
const pmsc = @import("internal/usb_pmsc.zig");

const tag = "USBPMSC";
const k_ra8_err_hw_init_failed: u16 = 0x201;

const pipe_bulk_in: u8 = 3;
const pipe_bulk_out: u8 = 4;
const ep_bulk_in: u8 = 1;
const ep_bulk_out: u8 = 2;
const ep_dir_out: u8 = 0;
const ep_dir_in: u8 = 1;
const ep_type_bulk: u8 = 0;

extern fn ra8_usb_device_init(speed: u8) u16;
extern fn ra8_usb_device_deinit(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;
extern fn ra8_usb_configure_endpoint(speed: u8, pipe: u8, ep: u8, dir: u8, ep_type: u8, max_packet: u16) u16;

/// `ra8_usb_pmsc_state_data_t g_usb_pmsc_state`, shared with usb_pmsc_scsi_abi.zig
/// and read by the C tests through ra8_usb_pmsc_internal.h.
export var g_usb_pmsc_state: pmsc.State = std.mem.zeroes(pmsc.State);

comptime {
    const host = @sizeOf(usize) == 8;
    const size: usize = if (host) 80 else 56;
    const cdb: usize = if (host) 58 else 34;
    if (@sizeOf(pmsc.State) != size) @compileError("ra8_usb_pmsc_state_data_t size drifted");
    if (@offsetOf(pmsc.State, "cbw_cdb") != cdb) @compileError("cbw_cdb offset drifted");
    if (@offsetOf(pmsc.State, "last_data_len") != size - 4) @compileError("last_data_len offset drifted");
}

fn nullPtr(msg: [*:0]const u8) u16 {
    common.ra8_log_emit_error(tag, msg);
    return common.k_ra8_err_null_ptr;
}

export fn priv_zero_bytes(dst: [*]u8, len: u32) void {
    @memset(dst[0..len], 0);
}

export fn ra8_usb_pmsc_feed_cbw(cbw: ?[*]const u8) u16 {
    const c = cbw orelse return nullPtr("feed_cbw: cbw");
    return pmsc.feedCbw(&g_usb_pmsc_state, c);
}

export fn ra8_usb_pmsc_dispatch_command(data_buf: ?[*]u8, data_buf_capacity: u32, data_len: ?*u32, csw_status: ?*u8) u16 {
    const buf = data_buf orelse return nullPtr("dispatch: data_buf");
    const len = data_len orelse return nullPtr("dispatch: data_len");
    const csw = csw_status orelse return nullPtr("dispatch: csw_status");
    return pmsc.dispatch(&g_usb_pmsc_state, buf, data_buf_capacity, len, csw);
}

export fn ra8_usb_pmsc_build_csw(csw_status: u8, residue: u32, out_csw: ?[*]u8) u16 {
    const out = out_csw orelse return nullPtr("build_csw: out_csw");
    return pmsc.buildCsw(&g_usb_pmsc_state, csw_status, residue, out);
}

export fn ra8_usb_pmsc_step() u16 {
    return pmsc.step(&g_usb_pmsc_state);
}

export fn ra8_usb_pmsc_attach_storage(storage: ?*const pmsc.Storage) u16 {
    if (!g_usb_pmsc_state.initialized) return common.k_ra8_err_invalid_state;
    const s = storage orelse return nullPtr("attach_storage: storage");
    if (s.read_block == null) return nullPtr("attach_storage: read_block");
    if (s.write_block == null) return nullPtr("attach_storage: write_block");
    if (s.get_capacity == null) return nullPtr("attach_storage: get_capacity");
    if (s.get_inquiry == null) return nullPtr("attach_storage: get_inquiry");
    pmsc.attach(&g_usb_pmsc_state, s);
    common.ra8_log_emit_info(tag, "storage attached");
    return common.k_ra8_ok;
}

export fn ra8_usb_pmsc_init(speed: u8) u16 {
    if (speed != pmsc.speed_fs and speed != pmsc.speed_hs) return common.k_ra8_err_invalid_arg;
    const usb_err = ra8_usb_device_init(speed);
    if (usb_err != common.k_ra8_ok) {
        common.ra8_log_emit_error_val(tag, "ra8_usb_device_init failed", usb_err);
        return k_ra8_err_hw_init_failed;
    }
    pmsc.resetForInit(&g_usb_pmsc_state, speed);
    const mp = pmsc.bulkMaxPacket(speed);
    _ = ra8_usb_configure_endpoint(speed, pipe_bulk_in, ep_bulk_in, ep_dir_in, ep_type_bulk, mp);
    _ = ra8_usb_configure_endpoint(speed, pipe_bulk_out, ep_bulk_out, ep_dir_out, ep_type_bulk, mp);
    common.ra8_log_emit_info_val(tag, "device-MSC ready", speed);
    return common.k_ra8_ok;
}

export fn ra8_usb_pmsc_close() u16 {
    if (!g_usb_pmsc_state.initialized) return common.k_ra8_err_invalid_state;
    const speed = g_usb_pmsc_state.speed;
    _ = ra8_usb_device_attach(speed, false);
    const err = ra8_usb_device_deinit(speed);
    pmsc.markClosed(&g_usb_pmsc_state);
    return err;
}
