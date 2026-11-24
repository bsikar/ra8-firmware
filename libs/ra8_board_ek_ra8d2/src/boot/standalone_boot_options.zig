//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Default `boot_options` values for standalone syntax/type checking.

pub const trust_zone: bool = false;
pub const cache_mpu: bool = false;
pub const mpu_via_hal: bool = false;
pub const cache_via_hal: bool = false;
pub const cgc_before_tz: bool = false;
