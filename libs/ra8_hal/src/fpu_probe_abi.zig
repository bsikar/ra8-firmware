//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for inc/ra8_fpu_probe.h: the double-precision FPU codegen witness.
//! The arithmetic is deliberately tiny so the object's disassembly shows how
//! the target lowers f64. The default cortex_m85 (fpv5-sp-d16) build emits
//! __aeabi_dmul / __aeabi_dadd calls. An RA8P1_DP_FPU=ON build, whose Zig cpu
//! carries fp_armv8d16, emits .f64 opcodes. Host builds run it on native
//! binary64 hardware.

export fn ra8_fpu_dp_madd(a: f64, b: f64, c: f64) f64 {
    return (a * b) + c;
}
