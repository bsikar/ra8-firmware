//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `inc/ra8_reconcile.h`: desired-state companion version
//! reconciliation over `"RA8U"` bundle descriptors and injected hooks.

const Err = @import("err").Err;
const bundle = @import("bundle");
const rec = @import("reconcile");

pub const Policy = rec.Policy;
pub const TargetNv = rec.TargetNv;
pub const NvState = rec.NvState;
pub const Report = rec.Report;
pub const Ops = rec.Ops;

export fn ra8_reconcile_nv_init(
    bundle_version: u32,
    descs_opt: ?[*]const bundle.ImageDesc,
    image_count: u16,
    out_state_opt: ?*NvState,
) callconv(.c) u16 {
    const out_state = out_state_opt orelse return Err.null_ptr.raw();
    const descs = descs_opt orelse return Err.null_ptr.raw();
    if (image_count == 0 or image_count > bundle.max_images) return Err.invalid_arg.raw();
    return rec.nvInit(bundle_version, descs[0..image_count], out_state);
}

export fn ra8_reconcile_nv_validate(state_opt: ?*const NvState) callconv(.c) u16 {
    const state = state_opt orelse return Err.null_ptr.raw();
    return rec.validateNv(state);
}

export fn ra8_reconcile_compute_backoff_ms(
    policy_opt: ?*const Policy,
    retries_used: u8,
    out_delay_opt: ?*u32,
) callconv(.c) u16 {
    const out_delay = out_delay_opt orelse return Err.null_ptr.raw();
    const policy = policy_opt orelse return Err.null_ptr.raw();
    const delay = rec.computeBackoffMs(policy, retries_used) catch return Err.invalid_arg.raw();
    out_delay.* = delay;
    return Err.ok.raw();
}

export fn ra8_reconcile_boot(
    bundle_hdr_opt: ?*const bundle.Header,
    descs_opt: ?[*]const bundle.ImageDesc,
    policy_opt: ?*const Policy,
    ops_opt: ?*const Ops,
    out_report_opt: ?*Report,
) callconv(.c) u16 {
    const out_report = out_report_opt orelse return Err.null_ptr.raw();
    const bundle_hdr = bundle_hdr_opt orelse return Err.null_ptr.raw();
    const descs = descs_opt orelse return Err.null_ptr.raw();
    const policy = policy_opt orelse return Err.null_ptr.raw();
    const ops = ops_opt orelse return Err.null_ptr.raw();
    return rec.boot(bundle_hdr, descs, policy, ops, out_report);
}
