//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! C ABI for ra8_dotf_open / close / set_region_window / enter_stop /
//! exit_stop (RA8FW-581). The DOTF primitives stay in ra8_dotf.c.

const common = @import("abi_common.zig");
const power = @import("internal/dotf_power.zig");

const tag = "DOTF";

extern fn ra8_dotf_init() u16;
extern fn ra8_dotf_deinit() u16;
extern fn ra8_dotf_install_key(channel: u8, handle: *const power.KeyHandle) u16;
extern fn ra8_dotf_set_iv(channel: u8, iv_words: [*]const u32) u16;
extern fn ra8_dotf_set_region(channel: u8, region: *const power.Region) u16;
extern fn ra8_dotf_select_region(channel: u8, region_id: u8) u16;
extern fn ra8_dotf_set_sca_level(channel: u8, level: u8) u16;
extern fn ra8_dotf_enable(channel: u8) u16;
extern fn ra8_mstp_enable(id: u16) u16;
extern fn ra8_mstp_disable(id: u16) u16;

/// Binds the orchestration in internal/dotf_power.zig to the C primitives.
const C = struct {
    pub fn init(_: C) u16 {
        return ra8_dotf_init();
    }
    pub fn installKey(_: C, ch: u8, key: *const power.KeyHandle) u16 {
        return ra8_dotf_install_key(ch, key);
    }
    pub fn setIv(_: C, ch: u8, iv: *const [power.iv_word_count]u32) u16 {
        return ra8_dotf_set_iv(ch, iv);
    }
    pub fn setRegion(_: C, ch: u8, region: *const power.Region) u16 {
        return ra8_dotf_set_region(ch, region);
    }
    pub fn selectRegion(_: C, ch: u8, id: u8) u16 {
        return ra8_dotf_select_region(ch, id);
    }
    pub fn setScaLevel(_: C, ch: u8, level: u8) u16 {
        return ra8_dotf_set_sca_level(ch, level);
    }
    pub fn enable(_: C, ch: u8) u16 {
        return ra8_dotf_enable(ch);
    }
    pub fn mstpEnable(_: C, id: u16) u16 {
        return ra8_mstp_enable(id);
    }
    pub fn mstpDisable(_: C, id: u16) u16 {
        return ra8_mstp_disable(id);
    }
    pub fn fail(_: C, msg: [*:0]const u8, err: u16) void {
        common.ra8_log_emit_error(tag, msg);
        common.ra8_log_emit_error_val(tag, "Error", err);
    }
};

export fn ra8_dotf_open(cfg: ?*const power.OpenCfg) u16 {
    const c = cfg orelse {
        common.ra8_log_emit_error(tag, "cfg must not be nullptr");
        return common.k_ra8_err_null_ptr;
    };
    return power.open(C{}, c);
}

export fn ra8_dotf_close() u16 {
    // Symmetric companion to ra8_dotf_open. HUM Ch 45.6.1 p 3050.
    return ra8_dotf_deinit();
}

export fn ra8_dotf_set_region_window(channel: u8, start: u32, len: u32) u16 {
    return power.setRegionWindow(C{}, channel, start, len);
}

export fn ra8_dotf_enter_stop() u16 {
    return power.enterStop(C{});
}

export fn ra8_dotf_exit_stop() u16 {
    return power.exitStop(C{});
}
