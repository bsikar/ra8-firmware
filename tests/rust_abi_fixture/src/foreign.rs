// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Unsafe C declarations and pointer operations, isolated from safe logic.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use std::sync::Mutex;
use std::sync::atomic::{AtomicU32, Ordering};

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
static APPLY_CALLS: AtomicU32 = AtomicU32::new(0);
static LIVE_HANDLES: AtomicU32 = AtomicU32::new(0);

/// Apply the fixed-layout operation and publish output only on success.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn ra8_rust_abi_fixture_apply(
    config: *const AbiConfig,
    out_result: *mut u32,
) -> AbiResult {
    APPLY_CALLS.fetch_add(1, Ordering::Relaxed);
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
    LIVE_HANDLES.store(1, Ordering::Relaxed);
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
    LIVE_HANDLES.store(0, Ordering::Relaxed);
    // SAFETY: equality with the one recorded Box address proves unique ownership.
    drop(unsafe { Box::from_raw(handle) });
    // SAFETY: the public contract requires writable pointer storage.
    unsafe { in_out_handle.write(std::ptr::null_mut()) };
    AbiResult::Ok
}

/// Reset the downstream-call counter used by chained ABI acceptance tests.
#[unsafe(no_mangle)]
pub extern "C" fn ra8_rust_abi_fixture_test_reset_apply_calls() {
    APPLY_CALLS.store(0, Ordering::Relaxed);
}

/// Return the number of Rust apply entries since the last reset.
#[unsafe(no_mangle)]
pub extern "C" fn ra8_rust_abi_fixture_test_apply_calls() -> u32 {
    APPLY_CALLS.load(Ordering::Relaxed)
}

/// Return the number of Rust-owned handles currently live.
#[unsafe(no_mangle)]
pub extern "C" fn ra8_rust_abi_fixture_test_live_handles() -> u32 {
    LIVE_HANDLES.load(Ordering::Relaxed)
}
