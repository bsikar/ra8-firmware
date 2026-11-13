//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Bump arena behind xz-embedded's allocator seam, in Zig behind the
//! unchanged unarch_xz_pool.h ABI (formerly src/unarch_xz_pool.c).
//!
//! The arena arithmetic is not written here: one module-static
//! installed scratch holds the store and every entry point forwards to
//! the shared decoder-scratch contract (`ra8_imgdec_scratch`, issue
//! RA8FW-308), which is where rounding, capacity and cursor accounting
//! live for every image-path bump arena in the tree. What stays local is
//! the policy this seam publishes and the contract does not have: the
//! install-time alignment precondition, the fail-closed refusal of a
//! second concurrent install, and the notion of being *uninstalled* at
//! all (the contract has no unbind, so `reset` drains through it and
//! then drops the store).

const std = @import("std");

/// The seam's published alignment (k_unarch_xz_pool_align): the promise
/// the vendored decoder's uint64_t-bearing structs are held to.
const pool_align: usize = 8;
/// The shared scratch contract's alignment (k_ra8_imgdec_scratch_align):
/// every bump is rounded up to it, so blocks land at least this aligned.
const scratch_align: usize = 16;

comptime {
    // The seam's published alignment may never exceed what now keeps it.
    // The header promises pool_align storage to the decoder; that promise
    // is kept by the shared contract's rounding, so the two constants are
    // pinned against each other rather than left to drift.
    if (scratch_align < pool_align) {
        @compileError("the shared scratch must align at least as strictly as the XZ pool promises");
    }
}

const k_ra8_ok: u16 = 0;
const k_ra8_err_invalid_size: u16 = 0x105;
const k_ra8_err_busy: u16 = 0x109;
const k_ra8_err_null_ptr: u16 = 0x504;

// The shared decoder-scratch contract (RA8FW-308). At app level the
// production implementation in libs/ra8_imgdec exports these; the package
// test builds the same contract test-side (src/unarch_xz_teststub.zig).
extern fn ra8_imgdec_scratch_init(s: *Scratch, base: ?*anyopaque, cap: usize) callconv(.c) u16;
extern fn ra8_imgdec_scratch_reset(s: *Scratch) callconv(.c) void;
extern fn ra8_imgdec_scratch_alloc(s: *Scratch, bytes: usize) callconv(.c) ?*anyopaque;

/// ra8_imgdec_scratch_t, the contract's record (libs/ra8_imgdec_scratch.h).
const Scratch = extern struct {
    base: ?[*]u8 = null,
    cap: usize = 0,
    offset: usize = 0,
    live: usize = 0,
    high_water: usize = 0,
};

/// The installed arena, or an all-zero (uninstalled) scratch.
/// `base == null` is the uninstalled state: allocations are refused and
/// `used` reports zero. Installed, every bump is served by the shared
/// contract's alloc, so the cursor invariant is the contract's, not this
/// file's. Module-private; mutate only through the install/reset API.
var s_pool: Scratch = .{};

/// unarch_xz_pool_install: bind [base, base + len) as the arena.
/// Refused when the base is null or misaligned, the length is zero, or an
/// arena is already installed (strictly nested wrapper usage), so two
/// concurrent XZ decodes cannot silently share state. On any error the
/// pool state is unchanged.
pub export fn unarch_xz_pool_install(base: ?*anyopaque, len: usize) callconv(.c) u16 {
    if (base == null) {
        return k_ra8_err_null_ptr;
    }
    if (len == 0) {
        return k_ra8_err_invalid_size;
    }
    if (@intFromPtr(base.?) % pool_align != 0) {
        return k_ra8_err_invalid_size;
    }
    if (s_pool.base != null) {
        return k_ra8_err_busy;
    }
    return ra8_imgdec_scratch_init(&s_pool, base, len);
}

/// unarch_xz_pool_reset: drain through the shared contract and then drop
/// the store, so the pool reads as uninstalled. Idempotent: calling with
/// no arena installed is a no-op. The contract's high-water mark survives,
/// so a session's peak can still be read for sizing after the store is gone.
pub export fn unarch_xz_pool_reset() callconv(.c) void {
    ra8_imgdec_scratch_reset(&s_pool);
    s_pool.base = null;
    s_pool.cap = 0;
}

/// unarch_xz_pool_alloc: bump-allocate size bytes from the installed arena
/// via the shared contract, which returns aligned storage or NULL when no
/// arena is installed, the request is zero, or the space is exhausted.
/// xz-embedded treats NULL as allocation failure and aborts cleanly, so
/// exhaustion fails closed. Frees are no-ops (arena semantics).
pub export fn unarch_xz_pool_alloc(size: u32) callconv(.c) ?*anyopaque {
    return ra8_imgdec_scratch_alloc(&s_pool, size);
}

/// unarch_xz_pool_used: bytes currently bump-allocated from the installed
/// arena (zero when no arena is installed). Introspection for tests and
/// RAM-high-water assertions.
pub export fn unarch_xz_pool_used() callconv(.c) u32 {
    return @intCast(s_pool.offset);
}
