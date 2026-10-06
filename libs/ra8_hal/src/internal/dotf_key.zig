//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! DOTF key and IV staging and key rotation (RA8FW-840, was part of
//! ra8_dotf.c). Pure over a ChanState and a register sink with writeReg00 and
//! writeReg03; the exports are in src/dotf_key_abi.zig.

const state = @import("dotf_state.zig");
const ctrl = @import("dotf_ctrl.zig");
/// Re-exported so host tests share the same types and constants.
pub const state_mod = state;
pub const ctrl_mod = ctrl;

pub const ok: u16 = 0;
pub const invalid_arg: u16 = 0x103;
pub const invalid_state: u16 = 0x104;

pub const Iv = [state.iv_words]u32;

/// AES key length in 32-bit words; anything not 192/256 is 128.
pub fn keyWords(size: u32) u8 {
    if (size == ctrl.key_size_192) return 6;
    if (size == ctrl.key_size_256) return 8;
    return 4;
}

/// A handle the hardware can take: marked valid, with a real key size.
pub fn validHandle(h: *const state.KeyHandle) bool {
    return h.valid != 0 and ctrl.validKeySize(h.size);
}

/// REG03 is the AES staging window (HUM 45.3 p 3049). FSP's r_ospi_b.c
/// feeds it big-endian, so each word is byte-swapped.
pub fn stageKey(regs: anytype, h: *const state.KeyHandle) void {
    for (h.words[0..keyWords(h.size)]) |w| regs.writeReg03(@byteSwap(w));
}

/// Counter = {IV[127:28], Address[31:4]} (HUM 45.1 p 3048).
pub fn stageIv(regs: anytype, iv: *const Iv) void {
    for (iv) |w| regs.writeReg03(@byteSwap(w));
}

/// ra8_dotf_install_key after the null and channel checks.
pub fn installKey(st: *state.ChanState, regs: anytype, h: *const state.KeyHandle) u16 {
    if (!validHandle(h)) return invalid_arg;
    st.key = h.*;
    st.cached_key_size = h.size;
    stageKey(regs, h);
    return ok;
}

/// ra8_dotf_set_iv after the null and channel checks.
pub fn setIv(st: *state.ChanState, regs: anytype, iv: *const Iv) void {
    st.iv_cache = iv.*;
    st.iv_valid = 1;
    stageIv(regs, iv);
}

/// internal_validate_rotate_inputs after the channel check.
pub fn validateRotate(st: *const state.ChanState, h: *const state.KeyHandle) u16 {
    if (!validHandle(h)) return invalid_arg;
    if (st.active_region_id == state.no_region) return invalid_state;
    return ok;
}

/// ra8_dotf_rotate_key once validated: quiesce REG00, restage the key, then
/// the new IV (or the cached one, or none), and re-arm only if it was armed.
pub fn rotate(st: *state.ChanState, regs: anytype, h: *const state.KeyHandle, iv: ?*const Iv) void {
    const was_enabled = st.enabled;
    ctrl.disable(st, regs);
    st.key = h.*;
    st.cached_key_size = h.size;
    stageKey(regs, h);
    if (iv) |new_iv| {
        setIv(st, regs, new_iv);
    } else if (st.iv_valid != 0) {
        stageIv(regs, &st.iv_cache);
    }
    if (was_enabled != 0) ctrl.enable(st, regs);
}
