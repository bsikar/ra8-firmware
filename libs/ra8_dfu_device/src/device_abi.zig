//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for `inc/ra8_dfu_device.h`: registers the vendored USBX DFU
//! class and backs its callbacks with the `ra8_dfu` MRAM slot programmer.
//!
//! The callbacks program each 64-byte block synchronously; the caller's
//! worker thread only commits the header after end-of-download, through
//! `ra8_dfu_device_worker_step`. DFU_UPLOAD reads straight out of the
//! target slot.
//!
//! `ra8_dfu`, the HAL and USBX are reached through their C ABIs, so an app
//! that names this archive also links those.

const usbx = @import("usbx");
const device = @import("device");

/// `ra8_err_t` values this membrane returns itself.
const err_invalid_state: u16 = 0x104;
const err_null_ptr: u16 = 0x504;

/// Class registration arguments, in the order the C passed them.
const reg_interface: c_ulong = 1;
const reg_config: c_ulong = 0;

extern fn ra8_dfu_program_prepare(target: u8) u16;
extern fn ra8_dfu_program_image(target: u8, offset: u32, data: [*]const u8, len: u32) u16;
extern fn ra8_dfu_program_commit(target: u8, img_len: u32, seq: u32) u16;
extern fn ra8_dfu_slot_seq(which: u8, out_seq: *u32) u16;
extern fn ra8_dfu_other_slot(which: u8) u8;
extern fn ra8_dfu_slot_base(which: u8) usize;
extern fn ux_dcd_ra8_usb_initialize(speed: u8) u16;
extern fn ra8_usb_device_attach(speed: u8, attached: bool) u16;

/// The single device instance, shared by the USBX callbacks and the worker.
var state: device.State = .{};
var class_name = "ux_slave_class_dfu".*;

/// Binds `device.State` to the real `ra8_dfu` programmer.
const Mram = struct {
    pub fn image(_: Mram, target: device.Slot, offset: u32, bytes: []const u8) u16 {
        return ra8_dfu_program_image(@intFromEnum(target), offset, bytes.ptr, @intCast(bytes.len));
    }

    pub fn commit(_: Mram, target: device.Slot, img_len: u32, seq: u32) u16 {
        return ra8_dfu_program_commit(@intFromEnum(target), img_len, seq);
    }

    /// Zero when the other slot has no valid header, as the C did.
    pub fn otherSeq(_: Mram, target: device.Slot) u32 {
        var seq: u32 = 0;
        _ = ra8_dfu_slot_seq(ra8_dfu_other_slot(@intFromEnum(target)), &seq);
        return seq;
    }
};

fn mediaStatus() c_ulong {
    return if (state.mediaOk()) usbx.media_status_ok else usbx.media_status_error;
}

/// State lives here, so there is nothing to set up or tear down; a
/// deactivate keeps the captured image for the worker to inspect.
fn onActivate(_: ?*anyopaque) callconv(.c) void {}

fn onWrite(_: ?*anyopaque, block: c_ulong, data: [*]u8, length: c_ulong, status: *c_ulong) callconv(.c) c_uint {
    const len: usize = @intCast(@min(length, device.block_bytes));
    state.write(Mram{}, @truncate(block), data[0..len]);
    status.* = mediaStatus();
    return usbx.success;
}

fn onRead(_: ?*anyopaque, block: c_ulong, data: [*]u8, length: c_ulong, actual: *c_ulong) callconv(.c) c_uint {
    const span = state.readSpan(@truncate(block), @truncate(length)) orelse {
        actual.* = 0;
        return usbx.success;
    };
    const src: [*]const u8 = @ptrFromInt(ra8_dfu_slot_base(@intFromEnum(state.target)) + span.offset);
    @memcpy(data[0..span.len], src[0..span.len]);
    actual.* = span.len;
    return usbx.success;
}

fn onGetStatus(_: ?*anyopaque, status: *c_ulong) callconv(.c) c_uint {
    status.* = mediaStatus();
    return usbx.success;
}

fn onNotify(_: ?*anyopaque, notification: c_ulong) callconv(.c) c_uint {
    if (notification == usbx.notification_end_download) state.manifest = true;
    return usbx.success;
}

fn registerClass(framework: [*]u8, framework_len: u32) c_uint {
    var parameter = usbx.DfuParameter{
        .will_detach = 0,
        .capabilities = usbx.capability_can_download | usbx.capability_can_upload,
        .instance_activate = onActivate,
        .instance_deactivate = onActivate,
        .read = onRead,
        .write = onWrite,
        .get_status = onGetStatus,
        .notify = onNotify,
        .framework = framework,
        .framework_length = framework_len,
    };
    return usbx._ux_device_stack_class_register(&class_name, usbx._ux_device_class_dfu_entry, reg_interface, reg_config, &parameter);
}

export fn ra8_dfu_device_set_target(target: u8) void {
    state.setTarget(target);
}

export fn ra8_dfu_device_start(
    speed: u8,
    pool: ?*anyopaque,
    pool_bytes: u32,
    framework: ?[*]u8,
    framework_len: u32,
    strings: ?[*]u8,
    strings_len: u32,
    langids: ?[*]u8,
    langids_len: u32,
) u16 {
    const fw = framework orelse return err_null_ptr;
    if (pool == null or strings == null or langids == null) return err_null_ptr;
    if (usbx._ux_system_initialize(pool, pool_bytes, null, 0) != usbx.success) return err_invalid_state;
    if (usbx._ux_device_stack_initialize(null, 0, fw, framework_len, strings, strings_len, langids, langids_len, null) != usbx.success) {
        return err_invalid_state;
    }
    if (registerClass(fw, framework_len) != usbx.success) return err_invalid_state;
    const dcd_err = ux_dcd_ra8_usb_initialize(speed);
    if (dcd_err != device.ok) return dcd_err;
    // Opened here, in thread context, so the per-block program path never
    // runs the controller bring-up (which logs over UART) from a USB
    // control request.
    const prep_err = ra8_dfu_program_prepare(@intFromEnum(state.target));
    if (prep_err != device.ok) return prep_err;
    state.prepared = true;
    return ra8_usb_device_attach(speed, true);
}

export fn ra8_dfu_device_worker_step() u16 {
    return state.workerStep(Mram{});
}

export fn ra8_dfu_device_image_len() u32 {
    return state.img_len;
}

export fn ra8_dfu_device_block_writes() u32 {
    return state.writes;
}

export fn ra8_dfu_device_manifested() bool {
    return state.manifest;
}

export fn ra8_dfu_device_last_error() u16 {
    return state.prog_err;
}

export fn ra8_dfu_device_committed() bool {
    return state.committed;
}
