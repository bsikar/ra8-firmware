//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for seven one-line forwarders (RA8FW-712), moved out of ra8_ceu.c,
//! ra8_flash_irq.c, ra8_i3c.c, ra8_mipi_phy.c and ra8_ssie.c, plus
//! ra8_gpio_release from gpio.c. Prototypes stay
//! in the driver headers; every target stays in C. Struct arguments pass
//! through as opaque pointers.

extern fn ra8_ceu_capture_disarm() u16;
extern fn ra8_pin_validator_release(pin: u16) u16;
extern fn ra8_flash_init(cfg: ?*const anyopaque) u16;
extern fn ra8_flash_deinit() u16;
extern fn ra8_i3c_ibi_read(out_ibi: ?*anyopaque) u16;
extern fn ra8_mipi_phy_init(cfg: ?*const anyopaque) u16;
extern fn ra8_mipi_phy_deinit() u16;
extern fn ra8_ssie_set_thresholds(channel: u8, tx_threshold: u8, rx_threshold: u8) u16;

export fn ra8_ceu_capture_stop() u16 {
    return ra8_ceu_capture_disarm();
}

/// FSP R_MRAM_Open name for ra8_flash_init.
export fn ra8_flash_open(cfg: ?*const anyopaque) u16 {
    return ra8_flash_init(cfg);
}

/// FSP R_MRAM_Close name for ra8_flash_deinit.
export fn ra8_flash_close() u16 {
    return ra8_flash_deinit();
}

export fn ra8_i3c_ibi_drain(ibi: ?*anyopaque) u16 {
    return ra8_i3c_ibi_read(ibi);
}

export fn ra8_mipi_phy_enter_stop() u16 {
    return ra8_mipi_phy_deinit();
}

export fn ra8_mipi_phy_exit_stop(cfg: ?*const anyopaque) u16 {
    return ra8_mipi_phy_init(cfg);
}

export fn ra8_ssie_set_fifo_threshold(channel: u8, tx_threshold: u8, rx_threshold: u8) u16 {
    return ra8_ssie_set_thresholds(channel, tx_threshold, rx_threshold);
}

/// `pin` is ra8_port_pin_t (enum : uint16_t).
export fn ra8_gpio_release(pin: u16) u16 {
    return ra8_pin_validator_release(pin);
}
