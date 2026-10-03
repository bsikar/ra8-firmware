//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for ra8_icu_* (internal/icu.zig, RA8FW-538). Built as its own object in
//! libra8_hal.a (RA8FW-542) so an image links only the units it calls.

const common = @import("abi_common.zig");
const icu = @import("internal/icu.zig");
const k_ra8_ok = common.k_ra8_ok;
const k_ra8_err_invalid_arg = common.k_ra8_err_invalid_arg;
const k_ra8_err_null_ptr = common.k_ra8_err_null_ptr;
const ra8_log_emit_info = common.ra8_log_emit_info;
const ra8_log_emit_info_val = common.ra8_log_emit_info_val;
const ra8_log_emit_error = common.ra8_log_emit_error;

const icu_tag = "ICU";

/// `ra8_err_t ra8_icu_init(void)` (inc/ra8_icu.h).
export fn ra8_icu_init() u16 {
    ra8_log_emit_info(icu_tag, "ra8_icu_init");
    icu.init(icu.hardware());
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_configure_irq_pin(uint8_t irq_num, const ra8_icu_irq_cfg_t* cfg)`.
export fn ra8_icu_configure_irq_pin(irq_num: u8, cfg: ?*const icu.Cfg) u16 {
    const config = cfg orelse {
        ra8_log_emit_error(icu_tag, "irq cfg");
        return k_ra8_err_null_ptr;
    };
    icu.configureIrqPin(icu.hardware(), irq_num, config.*) catch return k_ra8_err_invalid_arg;
    ra8_log_emit_info_val(icu_tag, "irqcr pin", irq_num);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_read_irqcr(uint8_t irq_num, uint8_t* out_val)`.
export fn ra8_icu_read_irqcr(irq_num: u8, out_val: ?*u8) u16 {
    const out = out_val orelse {
        ra8_log_emit_error(icu_tag, "irqcr out");
        return k_ra8_err_null_ptr;
    };
    out.* = icu.readIrqcr(icu.hardware(), irq_num) catch return k_ra8_err_invalid_arg;
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_nmi_enable(uint32_t mask)`.
export fn ra8_icu_nmi_enable(mask: u32) u16 {
    icu.nmiEnable(icu.hardware(), mask);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_nmi_disable(uint32_t mask)`.
export fn ra8_icu_nmi_disable(mask: u32) u16 {
    icu.nmiDisable(icu.hardware(), mask);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_nmi_clear(uint32_t mask)`.
export fn ra8_icu_nmi_clear(mask: u32) u16 {
    icu.nmiClear(icu.hardware(), mask);
    return k_ra8_ok;
}

/// `ra8_err_t ra8_icu_nmi_status(uint32_t* out_status)`.
export fn ra8_icu_nmi_status(out_status: ?*u32) u16 {
    const out = out_status orelse {
        ra8_log_emit_error(icu_tag, "nmi status out");
        return k_ra8_err_null_ptr;
    };
    out.* = icu.nmiStatus(icu.hardware());
    return k_ra8_ok;
}
