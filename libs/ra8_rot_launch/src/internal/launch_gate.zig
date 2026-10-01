//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The default-deny rule the launch gate rests on, in one place so it cannot
//! drift between the gates that use it.

/// `ra8_err_t` success. The type is a C23 `enum : uint16_t`, so this is the
/// ABI value.
pub const ok: u16 = 0;

/// Whether a gate's verdict permits the launch to continue.
///
/// Only an exact success passes. Anything else denies, including a code this
/// build has never heard of: a verdict that cannot be recognised is a
/// verdict that cannot be trusted.
pub fn passed(verdict: u16) bool {
    return verdict == ok;
}
