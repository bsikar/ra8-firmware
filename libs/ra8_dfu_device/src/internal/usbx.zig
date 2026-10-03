//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The slice of the vendored USBX device stack the DFU glue calls, declared
//! by hand from the pinned headers (ux_system.h, ux_device_stack.h,
//! ux_device_class_dfu.h). USBX itself stays C: it is on the permanent
//! list.
//!
//! The tree supplies no ux_user.h, so UX_DEVICE_CLASS_DFU_CUSTOM_REQUEST_ENABLE
//! is never defined and the parameter block has no custom-request hook.

/// UX_SUCCESS.
pub const success: c_uint = 0;
pub const media_status_ok: c_ulong = 0;
pub const media_status_error: c_ulong = 2;
pub const notification_end_download: c_ulong = 0x2;
pub const capability_can_download: c_ulong = 0x01;
pub const capability_can_upload: c_ulong = 0x02;

pub const Activate = *const fn (?*anyopaque) callconv(.c) void;
pub const Read = *const fn (?*anyopaque, c_ulong, [*]u8, c_ulong, *c_ulong) callconv(.c) c_uint;
pub const Write = *const fn (?*anyopaque, c_ulong, [*]u8, c_ulong, *c_ulong) callconv(.c) c_uint;
pub const GetStatus = *const fn (?*anyopaque, *c_ulong) callconv(.c) c_uint;
pub const Notify = *const fn (?*anyopaque, c_ulong) callconv(.c) c_uint;
pub const ClassEntry = *const fn (?*anyopaque) callconv(.c) c_uint;
pub const SlaveChange = *const fn (c_ulong) callconv(.c) c_uint;

/// UX_SLAVE_CLASS_DFU_PARAMETER, field for field.
pub const DfuParameter = extern struct {
    will_detach: c_ulong,
    capabilities: c_ulong,
    instance_activate: Activate,
    instance_deactivate: Activate,
    read: Read,
    write: Write,
    get_status: GetStatus,
    notify: Notify,
    framework: [*]u8,
    framework_length: c_ulong,
};

comptime {
    // Ten word-sized members and no padding, on ARM and on the host.
    if (@sizeOf(DfuParameter) != 10 * @sizeOf(usize)) @compileError("DfuParameter layout");
}

pub extern fn _ux_system_initialize(
    regular_pool: ?*anyopaque,
    regular_size: c_ulong,
    cache_safe_pool: ?*anyopaque,
    cache_safe_size: c_ulong,
) c_uint;

pub extern fn _ux_device_stack_initialize(
    framework_high_speed: ?[*]u8,
    framework_high_speed_len: c_ulong,
    framework_full_speed: ?[*]u8,
    framework_full_speed_len: c_ulong,
    strings: ?[*]u8,
    strings_len: c_ulong,
    langids: ?[*]u8,
    langids_len: c_ulong,
    change: ?SlaveChange,
) c_uint;

pub extern fn _ux_device_stack_class_register(
    class_name: [*]u8,
    entry: ClassEntry,
    configuration_number: c_ulong,
    interface_number: c_ulong,
    parameter: ?*anyopaque,
) c_uint;

pub extern fn _ux_device_class_dfu_entry(command: ?*anyopaque) callconv(.c) c_uint;
