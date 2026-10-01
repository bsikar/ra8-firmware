//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The `ra8_err_t` codes the two GPT adapters return. Values match
//! `libs/ra8_core/inc/ra8_err.h`, which pins `ra8_err_t` to `uint16_t`, so
//! every op and extern here returns `u16`; a wider return would hand C
//! callers stale upper bits.

pub const Err = struct {
    pub const ok: u16 = 0;
    pub const invalid_arg: u16 = 0x103;
    pub const invalid_state: u16 = 0x104;
    pub const not_found: u16 = 0x106;
    pub const not_supported: u16 = 0x107;
    pub const busy: u16 = 0x109;
    pub const would_block: u16 = 0x10B;
};
