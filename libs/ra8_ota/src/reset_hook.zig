//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The weak default for `ra8_ota_system_reset_hook`, deliberately its own
//! translation unit.
//!
//! The C carried this as `__attribute__((weak))` inside `ra8_ota.c`, and the
//! override it exists for only works because the default lives in an archive
//! member of its own: a firmware image (or `tests/misc/src/test_ra8_ota.c`)
//! defines the symbol strongly, the linker then never pulls this member, and
//! the call in `ra8_ota_commit_and_reboot` binds to the strong definition.
//! Emitting the weak definition beside that call instead makes the call site
//! resolve locally, so the override silently stops being reached.

/// `ra8_ota_system_reset_hook`: no-op unless the image overrides it (on
/// hardware, with `NVIC_SystemReset`).
export fn ra8_ota_system_reset_hook() callconv(.c) void {
    // Intentionally empty.
}
