//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! OV5640 open seam: pairs one sensor output mode with the CEU capture
//! configuration that must receive it and binds the CEU capture source
//! behind it. Enforces in code the bring-up order apps used to hand-roll
//! (probe -> configure -> CEU bind), so an out-of-order bring-up or a
//! mismatched hand-written descriptor can no longer reach a silent frame
//! timeout.

const abi = @import("abi_types.zig");

pub const err = abi.err;

/// Re-exported so one test binary sees the same types the archive compiled.
pub const Source = abi.Source;

/// `ra8_ov5640_mode_t`: the sensor output mode's ABI width.
pub const Mode = u8;

extern fn ra8_ov5640_probe(dev: ?*anyopaque, out_id: ?*u16) callconv(.c) u16;
extern fn ra8_ov5640_configure(dev: ?*anyopaque, mode: Mode) callconv(.c) u16;
extern fn ra8_camera_source_ceu_init(source: ?*Source, state: ?*anyopaque, cfg: ?*const anyopaque) callconv(.c) u16;

/// Probe the sensor, program one validated output mode, then bind the CEU
/// capture source behind it. The three stages run in that order and stop at
/// the first failure: a probe or configure fault leaves the CEU unclaimed.
/// The probe verifies the OV5640 chip ID itself, so callers no longer re-check
/// it or remap its failure.
///
/// The board-side parallel-pin routing (for example
/// `ra8_board_camera_route_parallel_pins()`) belongs after this call and
/// before the first capture: it follows the sensor configuration, and the
/// bound CEU stays idle until the first capture.
pub export fn ra8_camera_ov5640_open(
    source: ?*Source,
    state: ?*anyopaque,
    sensor: ?*anyopaque,
    mode: Mode,
    cfg: ?*const anyopaque,
) callconv(.c) u16 {
    const handle = source orelse return err.null_ptr;
    const backend = state orelse return err.null_ptr;
    const dev = sensor orelse return err.null_ptr;
    const config = cfg orelse return err.null_ptr;

    var chip_id: u16 = 0;
    const probe_err = ra8_ov5640_probe(dev, &chip_id);
    if (probe_err != err.ok) return probe_err;

    const configure_err = ra8_ov5640_configure(dev, mode);
    if (configure_err != err.ok) return configure_err;

    return ra8_camera_source_ceu_init(handle, backend, config);
}
