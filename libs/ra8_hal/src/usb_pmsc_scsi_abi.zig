//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the PMSC SCSI handlers (RA8FW-594), kept for the C coverage
//! suite. g_usb_pmsc_state is defined in usb_pmsc_abi.zig (RA8FW-756); the logic
//! is in internal/usb_pmsc_scsi.zig.

const scsi = @import("internal/usb_pmsc_scsi.zig");

extern var g_usb_pmsc_state: scsi.State;

comptime {
    const host = @sizeOf(usize) == 8;
    if (host and @sizeOf(scsi.State) != 80) @compileError("ra8_usb_pmsc_state_data_t is 80 bytes on host");
    if (host and @offsetOf(scsi.State, "cbw_cdb") != 58) @compileError("cbw_cdb sits at +58 on host");
    if (host and @offsetOf(scsi.Storage, "ctx") != 32) @compileError("storage.ctx sits at +32 on host");
}

export fn priv_handle_inquiry(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    return scsi.inquiry(&g_usb_pmsc_state.storage, buf, capacity, out_len);
}

export fn priv_handle_read_capacity(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    return scsi.readCapacity(&g_usb_pmsc_state.storage, buf, capacity, out_len);
}

export fn priv_handle_request_sense(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    return scsi.requestSense(buf, capacity, out_len);
}

export fn priv_handle_mode_sense(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    return scsi.modeSense(buf, capacity, out_len);
}

export fn priv_handle_read10(buf: [*]u8, capacity: u32, out_len: *u32) u16 {
    return scsi.read10(&g_usb_pmsc_state.storage, &g_usb_pmsc_state.cbw_cdb, buf, capacity, out_len);
}

export fn priv_handle_write10(buf: [*]const u8, out_len: *u32) u16 {
    return scsi.write10(&g_usb_pmsc_state.storage, &g_usb_pmsc_state.cbw_cdb, buf, out_len);
}
