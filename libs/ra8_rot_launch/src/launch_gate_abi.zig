//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C membrane for `ra8_dfu_launch`: the copy-to-run hand-off with the root
//! of trust in front of it.
//!
//! This is the opt-in half of what `RA8_ENABLE_ROOT_OF_TRUST` used to switch
//! inside `ra8_dfu_launch.c`. Linking this archive IS the opt-in, exactly as
//! it is for the verifier itself; an app that links `ra8_dfu` alone gets
//! `ra8_dfu_launch_unverified` and no `ra8_dfu_launch` at all, so an app that
//! means to authenticate cannot silently fail to.
//!
//! Both gates default-deny: on any trailer, signature or version failure
//! nothing is copied, nothing is branched to, and control returns to the
//! caller's fallback path.

const gate = @import("launch_gate");

extern fn ra8_dfu_run_target_valid(entry: u32, img_len: u32) bool;
extern fn ra8_dfu_launch_unverified(src: usize, img_len: u32, entry: u32) void;
extern fn ra8_rot_trailer_after(image_base: ?*const anyopaque, body_len: u32) ?*const anyopaque;
extern fn ra8_rot_verify_image(body: ?[*]const u8, body_len: u32, trailer: ?*const anyopaque) u16;
extern fn ra8_rot_trailer_image_version(trailer: ?*const anyopaque) u32;
extern fn ra8_rot_antirollback_default_store() *const anyopaque;
extern fn ra8_rot_antirollback_verify(store: ?*const anyopaque, image_version: u32) u16;
extern fn ra8_rot_antirollback_verify_no_commit(store: ?*const anyopaque, image_version: u32) u16;

fn launchWithGate(src: usize, img_len: u32, entry: u32, is_trial: bool) void {
    if (src == 0) return;
    if (!ra8_dfu_run_target_valid(entry, img_len)) return;

    // The signed image is [ body (img_len) ][ trailer ], so the trailer sits
    // immediately after the body. A missing or malformed one denies.
    const trailer = ra8_rot_trailer_after(@ptrFromInt(src), img_len) orelse return;
    if (!gate.passed(ra8_rot_verify_image(@ptrFromInt(src), img_len, trailer))) return;

    // Only once the image is authentic is its version worth reading: a
    // downgrade, and an unreadable counter, both deny. On an unconfirmed trial
    // boot we enforce the existing floor without advancing it, so rollback to
    // the prior confirmed version remains valid if the trial fails.
    const store = ra8_rot_antirollback_default_store();
    const img_ver = ra8_rot_trailer_image_version(trailer);
    const arb_err = switch (gate.floorPolicyForLaunch(is_trial)) {
        .verify_and_commit => ra8_rot_antirollback_verify(store, img_ver),
        .verify_no_commit => ra8_rot_antirollback_verify_no_commit(store, img_ver),
    };
    if (!gate.passed(arb_err)) return;

    ra8_dfu_launch_unverified(src, img_len, entry);
}

export fn ra8_dfu_launch(src: usize, img_len: u32, entry: u32) void {
    launchWithGate(src, img_len, entry, false);
}

export fn ra8_dfu_launch_trial(src: usize, img_len: u32, entry: u32) void {
    launchWithGate(src, img_len, entry, true);
}
