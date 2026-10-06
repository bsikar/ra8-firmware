//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for the one-line `*_exit_stop` wrappers (RA8FW-710), moved out of
//! adc.c, ra8_ceu.c, ra8_eth.c, ra8_eth_gwca.c, ra8_glcdc.c and ra8_i3c.c (now Zig).
//! Prototypes stay in each driver header; module stop stays in C.

const ids = @import("internal/exit_stop.zig");

extern fn ra8_mstp_enable(id: u16) u16;

export fn ra8_adc_exit_stop() u16 {
    return ra8_mstp_enable(ids.adc16h);
}

export fn ra8_ceu_exit_stop() u16 {
    return ra8_mstp_enable(ids.ceu);
}

export fn ra8_eth_exit_stop() u16 {
    return ra8_mstp_enable(ids.eswm);
}

export fn ra8_eth_gwca_exit_stop() u16 {
    return ra8_mstp_enable(ids.eswm);
}

export fn ra8_glcdc_exit_stop() u16 {
    return ra8_mstp_enable(ids.glcdc);
}

export fn ra8_i3c_exit_stop() u16 {
    return ra8_mstp_enable(ids.i3c);
}
