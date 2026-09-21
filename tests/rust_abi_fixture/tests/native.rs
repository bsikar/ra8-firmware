// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

use ra8_rust_abi_fixture::{AbiConfig, AbiResult, apply_config};

#[test]
fn layout_matches_public_c_contract() {
    assert_eq!(size_of::<AbiConfig>(), 8);
    assert_eq!(align_of::<AbiConfig>(), 4);
    assert_eq!(std::mem::offset_of!(AbiConfig, value), 0);
    assert_eq!(std::mem::offset_of!(AbiConfig, factor), 4);
    assert_eq!(std::mem::offset_of!(AbiConfig, enabled), 6);
    assert_eq!(std::mem::offset_of!(AbiConfig, reserved0), 7);
}

#[test]
fn safe_provider_maps_success_and_errors() {
    let mut config = AbiConfig {
        value: 7,
        factor: 3,
        enabled: 1,
        reserved0: 0,
    };
    assert_eq!(apply_config(config), Ok(21));
    config.enabled = 0;
    assert_eq!(apply_config(config), Ok(7));
    config.enabled = 2;
    assert_eq!(apply_config(config), Err(AbiResult::InvalidArgument));
    config.enabled = 1;
    config.reserved0 = 1;
    assert_eq!(apply_config(config), Err(AbiResult::InvalidArgument));
    config.reserved0 = 0;
    config.value = u32::MAX;
    config.factor = 2;
    assert_eq!(apply_config(config), Err(AbiResult::InvalidSize));
}
