//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Emits the Non-Secure image's `ra8_ns_rot_header_t` record: 8 bytes at
//! `ns_base + 0x40`, magic then signed body length.
//!
//! The magic comes from the same constant the Secure verifier compares
//! against (`regs.NsRot.magic`). The length is only known once the image is
//! laid out, so the NS linker script publishes it as the absolute symbol
//! `g_ra8_ls_ns_signed_body_len` and this record stores that symbol's address:
//! one R_ARM_ABS32 that resolves at final link, exactly as the C did. Zig cannot
//! turn an extern's address into an integer at comptime, so the field is a
//! pointer; on the 32-bit target that is the same 4 bytes.
//!
//! This sits outside `src/` for the reason the C did: `ra8_add_app()` globs
//! `src/` into the Secure images that name this library, and a
//! `.ns_rot_header` section in a Secure ELF is an orphan its script never
//! places.

const regs = @import("tz_regs");

/// Linker-defined: its ADDRESS is the signed body length. Never read through.
extern const g_ra8_ls_ns_signed_body_len: u8;

const Record = extern struct {
    magic: u32,
    body_len: *const u8,
};

comptime {
    if (@sizeOf(usize) == 4 and @sizeOf(Record) != 8)
        @compileError("ra8_ns_rot_header_t must be 8 bytes on the 32-bit target");
}

export const g_ra8_ns_rot_header: Record linksection(".ns_rot_header") = .{
    .magic = regs.NsRot.magic,
    .body_len = &g_ra8_ls_ns_signed_body_len,
};
