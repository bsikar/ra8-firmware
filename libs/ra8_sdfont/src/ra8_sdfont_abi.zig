//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for `ra8_sdfont`: the one exported symbol behind the unchanged
//! `inc/ra8_sdfont.h`, and the published record layouts that cross it.
//!
//! The load sequence lives here because it is the order the two halves run in,
//! nothing more: the bus comes up, the volume mounts, `policy` decides, and
//! the volume is released whatever the outcome.

const std = @import("std");
const abi_types = @import("internal/abi_types.zig");
const bus = @import("internal/bus.zig");
const policy = @import("internal/policy.zig");
const shared = @import("internal/root.zig");

const err = shared.err;

/// `ra8_sdfont_source_t`, laid out and asserted in `internal/abi_types.zig`.
pub const Source = abi_types.Source;

/// `ra8_sdfont_cfg_t`, laid out and asserted in `internal/abi_types.zig`.
pub const Config = abi_types.Config;

/// The real filesystem behind `policy.Fs`. Each shim is one forwarding call,
/// which is why the policy is tested and this is not.
const volume = struct {
    extern fn ra8_fs_open(handle: *anyopaque, path: [*:0]const u8, mode: u8, out_file: *?policy.File) u16;
    extern fn ra8_fs_read(file: policy.File, buf: [*]u8, max_len: u32, out_got: *u32) u16;
    extern fn ra8_fs_close(file: policy.File) u16;
    extern fn ra8_fs_write_file(handle: *anyopaque, path: [*:0]const u8, data: [*]const u8, len: u32) u16;

    /// `k_ra8_fs_mode_read`.
    const mode_read: u8 = 0;

    fn openRead(ctx: ?*anyopaque, name: [*:0]const u8, out_file: *?policy.File) u16 {
        const handle = ctx orelse return err.null_ptr;
        return ra8_fs_open(handle, name, mode_read, out_file);
    }

    fn writeFile(ctx: ?*anyopaque, name: [*:0]const u8, data: [*]const u8, len: u32) u16 {
        const handle = ctx orelse return err.null_ptr;
        return ra8_fs_write_file(handle, name, data, len);
    }

    fn read(ctx: ?*anyopaque, file: policy.File, buf: [*]u8, cap: u32, out_got: *u32) u16 {
        _ = ctx;
        return ra8_fs_read(file, buf, cap, out_got);
    }

    fn close(ctx: ?*anyopaque, file: policy.File) u16 {
        _ = ctx;
        return ra8_fs_close(file);
    }
};

/// Load the configured font off the SD card, provisioning it first when the
/// card does not carry it and the caller offered a blob.
///
/// The NULL checks come before the capacity check, and both come before any
/// bus access, because that is the order callers rely on: a misconfigured app
/// gets a diagnosis rather than a powered card.
pub export fn ra8_sdfont_load(
    cfg: ?*const Config,
    buf: ?[*]u8,
    cap: u32,
    out_len: ?*u32,
    out_source: ?*Source,
) callconv(.c) u16 {
    const config = cfg orelse return err.null_ptr;
    const storage = buf orelse return err.null_ptr;
    const length = out_len orelse return err.null_ptr;

    const capacity_ok = policy.validateCapacity(cap);
    if (capacity_ok != err.ok) {
        return capacity_ok;
    }

    const up = bus.bringUpSpi(config.spi_channel, config.pclka_hz, .{
        .sck = config.sck,
        .cipo = config.cipo,
        .copi = config.copi,
        .cs = config.cs,
    });
    if (up != err.ok) {
        return up;
    }

    var handle: ?*anyopaque = null;
    const mounted = bus.mount(&handle);
    if (mounted != err.ok) {
        return mounted;
    }
    const volume_handle = handle orelse return err.null_ptr;
    defer bus.unmount(volume_handle);

    const fs: policy.Fs = .{
        .ctx = volume_handle,
        .open_read = volume.openRead,
        .write_file = volume.writeFile,
        .read = volume.read,
        .close = volume.close,
    };

    var file: ?policy.File = null;
    var source: Source = .card;
    var status = policy.openOrProvision(
        fs,
        policy.resolveName(config.filename),
        .{ .data = config.provision_blob, .len = config.provision_len },
        &file,
        &source,
    );
    if (status == err.ok) {
        const opened = file orelse return err.null_ptr;
        status = policy.readFont(fs, opened, storage, cap, length);
    }
    if (status != err.ok) {
        return status;
    }

    if (out_source) |sink| {
        sink.* = source;
    }
    return err.ok;
}
