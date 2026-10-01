//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_secure.h`.
//!
//! Both entry points treat a null pointer as the header describes rather
//! than as a precondition violation: the compare reports "not equal" and the
//! scrub is a no-op. Neither logs, because both are reachable from the
//! secure-boot and crypto paths where a log line about a failed digest
//! comparison would itself be a signal.
//!
//! The pointers arrive bare with a length, so this is where they become
//! slices; nothing below this file deals in raw pointers.

const compare = @import("secure_compare");
const scrub = @import("secure_scrub");

pub export fn ra8_ct_equal(a: ?*const anyopaque, b: ?*const anyopaque, len: usize) callconv(.c) bool {
    const lhs = a orelse return false;
    const rhs = b orelse return false;
    const first: [*]const u8 = @ptrCast(lhs);
    const second: [*]const u8 = @ptrCast(rhs);
    return compare.equal(first[0..len], second[0..len]);
}

pub export fn ra8_secure_memzero(ptr: ?*anyopaque, len: usize) callconv(.c) void {
    const target = ptr orelse return;
    const bytes: [*]u8 = @ptrCast(target);
    scrub.zeroize(bytes[0..len]);
}
