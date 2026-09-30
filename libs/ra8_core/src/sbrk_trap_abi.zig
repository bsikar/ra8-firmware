//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI membrane for `libs/ra8_core/inc/ra8_sbrk_trap.h` (#2895): the
//! newlib heap syscall, replaced by a halting trap.
//!
//! The exported name is bare `_sbrk`, unprefixed, in every build. It is not
//! a name this project chose: newlib's allocator calls it by that exact
//! spelling, so the definition has to answer to it or the override does not
//! happen. That is also why this file is in the GENERAL archive and not in
//! the freestanding one next to `memset` and `rand`, which is the archive
//! libc-named symbols usually belong in. Two reasons, and the second is the
//! real one:
//!
//!   1. The freestanding archive renames its whole surface under
//!      `-Dabi-prefix` so host suites can link it beside a real libc. A
//!      prefixed `_sbrk` answers nothing: newlib would not find it, and the
//!      host death test reaches the trap by its production spelling.
//!   2. The trap calls `ra8_fatal_error`. The freestanding archive is a
//!      self-contained libc subset with no `ra8_*` dependency, and the two
//!      suites that link its prefixed half alone would stop linking the
//!      moment it acquired one. `ra8_fatal_error` already lives in this
//!      archive (#2875), so the call stays inside one artifact.
//!
//! On a correctly built image this is unreachable: with zero heap callers
//! `--gc-sections` discards it, and the linker scripts define no `end`
//! anchor and no `.heap` section for it to grow into.

const heap = @import("heap_sbrk");

extern fn ra8_fatal_error(tag: [*:0]const u8, message: [*:0]const u8, err: u32) noreturn;

/// `incr` is ignored: the request is never satisfied, only reported.
fn sbrk(incr: isize) callconv(.c) ?*anyopaque {
    _ = incr;
    ra8_fatal_error(heap.policy.tag.ptr, heap.policy.message.ptr, heap.policy.err);
}

comptime {
    @export(&sbrk, .{ .name = "_sbrk", .linkage = .strong });
}
