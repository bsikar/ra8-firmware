//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the packed MSTP id decoders (RA8FW-704), moved out of
//! ra8_mstp.c. Prototypes stay in inc/ra8_mstp_regs.h.

const ids = @import("internal/mstp_ids.zig");

/// `ra8_mstp_reg_t ra8_mstp_id_reg(ra8_mstp_t)`; both enums are fixed-width.
export fn ra8_mstp_id_reg(id: u16) u8 {
    return ids.reg(id);
}

/// `uint8_t ra8_mstp_id_bit(ra8_mstp_t)`.
export fn ra8_mstp_id_bit(id: u16) u8 {
    return ids.bit(id);
}
