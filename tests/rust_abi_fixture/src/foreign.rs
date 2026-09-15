// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Unsafe C declarations and pointer operations, isolated from safe logic.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use std::sync::Mutex;

use crate::{AbiConfig, AbiResult, apply_config};

/// Opaque allocation whose representation is never exposed to C.
#[repr(C)]
pub struct AbiFixture {
    marker: u8,
}

#[derive(Default)]
struct Pool {
    live_address: Option<usize>,
    fail_next: bool,
}

static POOL: Mutex<Pool> = Mutex::new(Pool {
    live_address: None,
    fail_next: false,
});

/// Apply the fixed-layout operation and publish output only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ra8_rust_abi_fixture_apply(
    config: *const AbiConfig,
    out_result: *mut u32,
) -> AbiResult {
    if out_result.is_null() || config.is_null() {
        return AbiResult::NullPointer;
    }
    // SAFETY: null was rejected; the public contract requires readable config storage.
    let config = unsafe { config.read() };
    match apply_config(config) {
        Ok(value) => {
            // SAFETY: the public contract requires writable output storage.
            unsafe { out_result.write(value) };
            AbiResult::Ok
        }
        Err(error) => error,
    }
}

/// Make exactly the next fixture allocation fail.
#[unsafe(no_mangle)]
pub extern "C" fn ra8_rust_abi_fixture_test_fail_next_allocation() {
    if let Ok(mut pool) = POOL.lock() {
        pool.fail_next = true;
    }
}

/// Acquire the fixture's single bounded handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ra8_rust_abi_fixture_create(
    out_handle: *mut *mut AbiFixture,
) -> AbiResult {
    if out_handle.is_null() {
        return AbiResult::NullPointer;
    }
    let Ok(mut pool) = POOL.lock() else {
        return AbiResult::InvalidArgument;
    };
    if pool.fail_next {
        pool.fail_next = false;
        return AbiResult::NoMemory;
    }
    if pool.live_address.is_some() {
        return AbiResult::NoMemory;
    }
    let handle = Box::into_raw(Box::new(AbiFixture { marker: 0xA5 }));
    pool.live_address = Some(handle.addr());
    // SAFETY: the public contract requires writable handle storage.
    unsafe { out_handle.write(handle) };
    AbiResult::Ok
}

/// Release a live handle and clear the caller's slot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ra8_rust_abi_fixture_destroy(
    in_out_handle: *mut *mut AbiFixture,
) -> AbiResult {
    if in_out_handle.is_null() {
        return AbiResult::NullPointer;
    }
    // SAFETY: the public contract requires readable pointer storage.
    let handle = unsafe { in_out_handle.read() };
    if handle.is_null() {
        return AbiResult::Ok;
    }
    let Ok(mut pool) = POOL.lock() else {
        return AbiResult::InvalidArgument;
    };
    if pool.live_address != Some(handle.addr()) {
        return AbiResult::InvalidArgument;
    }
    pool.live_address = None;
    // SAFETY: equality with the one recorded Box address proves unique ownership.
    drop(unsafe { Box::from_raw(handle) });
    // SAFETY: the public contract requires writable pointer storage.
    unsafe { in_out_handle.write(std::ptr::null_mut()) };
    AbiResult::Ok
}
