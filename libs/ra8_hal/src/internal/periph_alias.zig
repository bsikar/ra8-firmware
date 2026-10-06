//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The IDAU bit[28]=1 Non-secure alias of a peripheral base (RA8FW-833).
//! A TrustZone Non-secure image reaches its peripherals at the Secure base
//! plus 0x1000_0000, which is what the C picks with RA8_PERIPH_NS_ALIAS in
//! inc/ra8_mstp_regs.h and inc/ra8_usb_regs.h. The ABI units pass the
//! archive's `periph_ns_alias` build option as `ns`.

/// IDAU attribution bit: set on every Non-secure peripheral alias.
pub const ns_bit: usize = 0x1000_0000;

/// The base an image uses: the Secure alias, or its Non-secure alias when `ns`.
pub fn of(secure_base: usize, ns: bool) usize {
    return if (ns) secure_base | ns_bit else secure_base;
}
