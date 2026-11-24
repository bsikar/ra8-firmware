//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Register addresses, key words and layout constants the secure boot writes.
//!
//! Every value here is a property of the RA8D2 silicon, not of this port, so
//! it is stated once and shared by the sequencer, the PSAR gate and the tests.

/// MMIO addresses the secure boot touches.
pub const Addr = struct {
    /// HUM Ch 3.2.1 "IPCSAR" p 205: IPC security attribution.
    pub const ipcsar: usize = 0x40008610;
    /// HUM Ch 3.2.2 "IPCPAR" p 208: IPC privilege attribution.
    pub const ipcpar: usize = 0x40008614;
    /// ARMv8-M ARM B3.2.4: Non-Secure alias of SCB.VTOR.
    pub const vtor_ns: usize = 0xE002ED08;
    /// HUM Ch 13.2.1 "PRCR_S" p 521: secure register write protection.
    pub const prcr_s: usize = 0x4001E3FA;
};

/// PRCR_S key and gate words. The upper byte is the write key; without it
/// the chip silently drops the store.
pub const Prcr = struct {
    /// Key with every protect bit clear.
    pub const key: u16 = 0xA500;
    /// PRC4 guards the CPSCU block.
    pub const prc4: u16 = 1 << 4;
    /// Key + PRC4 set: CPSCU writable.
    pub const open: u16 = key | prc4;
    /// Key alone: write protection restored.
    pub const close: u16 = key;
};

/// The Non-Secure root-of-trust header the NS linker emits.
pub const NsRot = struct {
    /// ASCII "NSR1", little-endian.
    pub const magic: u32 = 0x3152534E;
    /// Offset from the NS base, just past the 16-slot NS vector table.
    pub const header_offset: usize = 0x40;
};

/// Bounds the PSAR gate works within.
pub const Psar = struct {
    /// Read-back attempts before the gate reports a timeout.
    pub const readback_spins: u32 = 1000;
};

/// `ra8_err_t` is `enum : uint16_t`.
pub const ErrCode = u16;

/// Error codes shared with `ra8_err.h`, as the C ABI spells them.
pub const Err = struct {
    pub const ok: ErrCode = 0;
    pub const invalid_arg: ErrCode = 0x103;
    pub const invalid_size: ErrCode = 0x105;
    pub const not_supported: ErrCode = 0x107;
    pub const timeout: ErrCode = 0x108;
    pub const validation_failed: ErrCode = 0x501;
    pub const null_ptr: ErrCode = 0x504;
};
