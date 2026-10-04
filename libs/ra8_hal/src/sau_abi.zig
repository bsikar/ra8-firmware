//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI exports for the TrustZone SAU driver (internal/sau.zig,
//! RA8FW-571). Built as its own object in libra8_hal.a (RA8FW-542). The C
//! driver logged nothing, so neither does this.

const common = @import("abi_common.zig");
const sau = @import("internal/sau.zig");

const block = sau.Block{};

fn code(err: sau.Error) u16 {
    return switch (err) {
        error.NullPtr => common.k_ra8_err_null_ptr,
        error.InvalidArg => common.k_ra8_err_invalid_arg,
    };
}

/// `ra8_err_t ra8_sau_configure(const ra8_sau_cfg_t*)`.
export fn ra8_sau_configure(cfg: ?*const sau.Cfg) u16 {
    const c = sau.cfgValid(cfg, block.sregionCount()) catch |e| return code(e);
    block.install(c);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_sau_set_region(uint8_t, const ra8_sau_region_t*)`.
export fn ra8_sau_set_region(region: u8, region_cfg: ?*const sau.Region) u16 {
    const r = region_cfg orelse return common.k_ra8_err_null_ptr;
    if (region >= block.sregionCount()) return common.k_ra8_err_invalid_arg;
    if (!sau.regionValid(r)) return common.k_ra8_err_invalid_arg;
    block.writeRegion(region, r);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_sau_enable(void)`.
export fn ra8_sau_enable() u16 {
    block.setEnabled(true);
    return common.k_ra8_ok;
}

/// `ra8_err_t ra8_sau_disable(void)`.
export fn ra8_sau_disable() u16 {
    block.setEnabled(false);
    return common.k_ra8_ok;
}

/// `bool ra8_sau_is_enabled(void)`.
export fn ra8_sau_is_enabled() bool {
    return block.isEnabled();
}

/// `uint8_t ra8_sau_region_count(void)`.
export fn ra8_sau_region_count() u8 {
    return block.sregionCount();
}

/// `const ra8_sau_cfg_t* ra8_sau_boot_map(void)`.
export fn ra8_sau_boot_map() *const sau.Cfg {
    return &sau.boot_cfg;
}

/// `ra8_err_t ra8_sau_apply_boot_map(void)`.
export fn ra8_sau_apply_boot_map() u16 {
    if (block.sregionCount() < sau.boot_region_count) return common.k_ra8_err_invalid_arg;
    block.install(&sau.boot_cfg);
    return common.k_ra8_ok;
}
