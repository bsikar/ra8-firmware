//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The heap policy this firmware answers `_sbrk` with.
//!
//! There is no heap. Target firmware links `-nostdlib` with neither newlib
//! nor libnosys, so `malloc` and friends already fail closed at link time.
//! This is the defence behind that: if any legacy object or vendor routine
//! still resolves `_sbrk` and calls it, the answer is a halt, not an
//! unbounded bump allocator handing back storage that does not exist.
//!
//! The three values below are the whole policy, and they are here rather
//! than inline in the membrane because the host death test asserts every one
//! of them by hand (`tests/hal/src/test_ra8_sbrk_trap_cov.c`). A silent edit
//! to the tag or the message would leave that suite asserting against a
//! string the firmware no longer reports.

/// What the trap reports before the machine stops.
pub const policy = struct {
    /// Subsystem tag the fatal sink prints.
    pub const tag: [:0]const u8 = "SBRK";
    /// Why it stopped, in the words the sink emits.
    pub const message: [:0]const u8 = "_sbrk called -- firmware is heap-free";
    /// No numeric code: the call itself is the whole fault.
    pub const err: u32 = 0;
};
