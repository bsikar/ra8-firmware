//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The thumb-only half of the Ed25519 verify NSC veneer (RA8FW-291):
//! the privileged TT probe, the secure body behind the gate, and the
//! SG entry in `.gnu.sgstubs`. The host-testable decision logic lives
//! in the shared module; this file is imported there only on thumb
//! targets.

const std = @import("std");
const shared = @import("ra8_nsc_appimg_verify.zig");

const WireMsg = shared.WireMsg;
const k_err_invalid_arg = shared.k_err_invalid_arg;
const k_err_null_ptr = shared.k_err_null_ptr;

/// Test a [p, p+len) byte range as the Non-Secure state sees it,
/// privileged (TTA, the CMSE_NONSECURE probe).
fn ns_range_readable(p: u32, len: u32) bool {
    if (len == 0) return false;
    const last = p +% len -% 1;
    if (last < p) return false;
    const start_info = tta(p);
    if ((last ^ p) >= 32) {
        const last_info = tta(last);
        if (last_info != start_info) return false;
    }
    return (start_info & shared.tt_read_bit) != 0 and p != 0;
}

fn tta(addr: u32) u32 {
    var out: u32 = undefined;
    asm volatile ("tta %[out], %[in]"
        : [out] "=r" (out),
        : [in] "r" (addr),
    );
    return out;
}

/// The secure body behind the SG gate: validate every Non-Secure
/// pointer range before anything is read, then forward to the
/// production backend. The ranges mirror the C veneer contract's
/// RA8_NSC_CHECK_NS_RANGE_R on each buffer the backend touches: the
/// two-run message struct itself, both signed runs it names, the
/// 64-byte signature and the 32-byte public key. A range that is not
/// Non-Secure-readable refuses with k_ra8_err_invalid_arg before the
/// backend runs.
export fn ra8_nsc_appimg_ed25519_verify_body(
    ctx: ?*anyopaque,
    msg: ?*const WireMsg,
    signature: ?[*]const u8,
    public_key: ?[*]const u8,
) callconv(.c) u16 {
    const msg_ptr = msg orelse return k_err_null_ptr;
    const sig_ptr = signature orelse return k_err_null_ptr;
    const key_ptr = public_key orelse return k_err_null_ptr;

    const msg_addr: u32 = @intFromPtr(msg_ptr);
    const sig_addr: u32 = @intFromPtr(sig_ptr);
    const key_addr: u32 = @intFromPtr(key_ptr);

    if (!ns_range_readable(msg_addr, @sizeOf(WireMsg))) return k_err_invalid_arg;

    // The two runs live in Non-Secure memory the struct describes;
    // the lengths are read only after the struct itself checked out.
    const wire = msg_ptr.*;
    const head_addr: u32 = @intFromPtr(wire.head orelse return k_err_null_ptr);
    const tail_addr: u32 = @intFromPtr(wire.tail orelse return k_err_null_ptr);
    if (!ns_range_readable(head_addr, wire.head_len)) return k_err_invalid_arg;
    if (!ns_range_readable(tail_addr, wire.tail_len)) return k_err_invalid_arg;
    if (!ns_range_readable(sig_addr, 64)) return k_err_invalid_arg;
    if (!ns_range_readable(key_addr, 32)) return k_err_invalid_arg;

    return shared.ra8_appimg_ed25519_verify(ctx, msg, signature, public_key);
}

comptime {
    // The ACLE secure-entry symbol the two-image link binds: the
    // address of the instruction after the SG, one word past the
    // veneer symbol this file exports into .gnu.sgstubs.
    asm (".globl __acle_se_ra8_nsc_appimg_ed25519_verify\n" ++ ".set __acle_se_ra8_nsc_appimg_ed25519_verify, ra8_nsc_appimg_ed25519_verify + 4");
}
comptime {
    // The Non-Secure Callable entry itself, authored as module assembly so
    // the section, the 32-byte NSC alignment and the SG word are all
    // explicit. The prologue/epilogue mirror clang's expansion of a
    // cmse_nonsecure_entry wrapper exactly: the Non-Secure caller's r7/lr
    // are saved around the call, and on the way out r1-r3 and r12 are
    // scrubbed with the (Non-Secure-known) return address, the flags are
    // scrubbed the same way, and the return goes through bxns. r0 carries
    // the verdict back; r4-r11 need no scrub because the body preserves
    // them per the ABI. The backend is integer-only, so no FPU scrub is
    // needed; an FPU-using backend must add vscclrm before this veneer
    // ships with it.
    asm (
        \\.section .gnu.sgstubs,"ax",%progbits
        \\.p2align 5
        \\.global ra8_nsc_appimg_ed25519_verify
        \\.thumb_func
        \\.type ra8_nsc_appimg_ed25519_verify, %function
        \\ra8_nsc_appimg_ed25519_verify:
        \\.inst 0xE97FE97F // sg: the secure gateway
        \\push {r7, lr}
        \\bl ra8_nsc_appimg_ed25519_verify_body
        \\pop.w {r7, lr}
        \\mov r1, lr
        \\mov r2, lr
        \\mov r3, lr
        \\mov r12, lr
        \\msr apsr_nzcvq, lr
        \\bxns lr
        \\.section .text
    );
}
