// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Safe Rust ownership facade over the hand-authored public C ABI.
#![expect(
    unsafe_code,
    reason = "all unsafe operations are confined to this audited C ABI adapter"
)]

use std::marker::PhantomData;
use std::ptr::NonNull;
use std::rc::Rc;
use std::sync::{Mutex, MutexGuard};

use crate::AbiResult;

static ABI_LOCK: Mutex<()> = Mutex::new(());

fn lock() -> Result<MutexGuard<'static, ()>, AbiResult> {
    ABI_LOCK.lock().map_err(|_| AbiResult::State)
}

fn lock_for_drop() -> MutexGuard<'static, ()> {
    ABI_LOCK
        .lock()
        .unwrap_or_else(std::sync::PoisonError::into_inner)
}

#[repr(C)]
struct RawFixture {
    _private: [u8; 0],
}

/// Fixed-layout input matching `ra8_abi_fixture_config_t` in the public header.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(C)]
pub struct Config {
    /// Value to scale.
    pub value: u32,
    /// Unsigned scale factor.
    pub factor: u16,
    /// Canonical C ABI boolean; only zero and one are valid.
    pub enabled: u8,
    /// Reserved byte; callers supply zero.
    pub reserved0: u8,
}

unsafe extern "C" {
    fn ra8_abi_fixture_apply(config: *const Config, out_result: *mut u32) -> u16;
    fn ra8_abi_fixture_create(out_handle: *mut *mut RawFixture) -> u16;
    fn ra8_abi_fixture_destroy(in_out_handle: *mut *mut RawFixture) -> u16;
    fn ra8_abi_fixture_copy(
        handle: *mut RawFixture,
        input: *const u8,
        input_len: u32,
        output: *mut u8,
        capacity: u32,
        out_len: *mut u32,
    ) -> u16;
    fn ra8_abi_fixture_bytes_create(
        handle: *mut RawFixture,
        input: *const u8,
        input_len: u32,
        out_bytes: *mut *mut u8,
        out_len: *mut u32,
    ) -> u16;
    fn ra8_abi_fixture_bytes_release(in_out_bytes: *mut *mut u8) -> u16;
    fn ra8_abi_fixture_test_fail_next_allocation();
}

fn result(raw: u16) -> Result<(), AbiResult> {
    match AbiResult::try_from(raw) {
        Ok(AbiResult::Ok) => Ok(()),
        Ok(error) => Err(error),
        Err(_) => Err(AbiResult::InvalidArgument),
    }
}

fn length(value: usize) -> Result<u32, AbiResult> {
    u32::try_from(value).map_err(|_| AbiResult::Length)
}

/// Validate and scale one public fixed-layout value.
///
/// # Errors
///
/// Returns the stable error reported by the Zig provider.
pub fn apply(config: &Config) -> Result<u32, AbiResult> {
    let _guard = lock()?;
    let mut output = 0_u32;
    unsafe { result(ra8_abi_fixture_apply(config, &raw mut output))? };
    Ok(output)
}

/// Sole owner of one opaque handle created by the Zig provider.
pub struct Fixture {
    raw: Option<NonNull<RawFixture>>,
    _not_send: PhantomData<Rc<()>>,
}

impl Fixture {
    /// Acquire the fixture's bounded handle.
    ///
    /// # Errors
    ///
    /// Returns the provider error without acquiring ownership.
    pub fn create() -> Result<Self, AbiResult> {
        let guard = lock()?;
        Self::create_locked(&guard)
    }

    /// Exercise the fixture's next-allocation failure atomically.
    ///
    /// # Errors
    ///
    /// Returns [`AbiResult::NoMemory`] when the provider honors its test
    /// control. Any other provider result is returned unchanged.
    pub fn create_with_forced_allocation_failure_for_test() -> Result<Self, AbiResult> {
        let guard = lock()?;
        unsafe { ra8_abi_fixture_test_fail_next_allocation() };
        Self::create_locked(&guard)
    }

    fn create_locked(_guard: &MutexGuard<'static, ()>) -> Result<Self, AbiResult> {
        let mut raw = std::ptr::null_mut();
        unsafe { result(ra8_abi_fixture_create(&raw mut raw))? };
        let raw = NonNull::new(raw).ok_or(AbiResult::Null)?;
        Ok(Self {
            raw: Some(raw),
            _not_send: PhantomData,
        })
    }

    /// Copy borrowed input into caller-owned output.
    ///
    /// # Errors
    ///
    /// Returns [`AbiResult::Length`] for a Rust length outside `uint32_t` or
    /// the stable provider result. Failure leaves `output` unchanged.
    pub fn copy_into(&mut self, input: &[u8], output: &mut [u8]) -> Result<usize, AbiResult> {
        let input_len = length(input.len())?;
        let capacity = length(output.len())?;
        let mut out_len = 0_u32;
        let _guard = lock()?;
        unsafe {
            result(ra8_abi_fixture_copy(
                self.raw.ok_or(AbiResult::State)?.as_ptr(),
                input.as_ptr(),
                input_len,
                output.as_mut_ptr(),
                capacity,
                &raw mut out_len,
            ))?;
        }
        Ok(out_len as usize)
    }

    /// Create one library-owned byte result tied to this handle borrow.
    ///
    /// # Errors
    ///
    /// Returns the stable provider result without publishing ownership.
    pub fn owned_bytes<'fixture>(
        &'fixture mut self,
        input: &[u8],
    ) -> Result<OwnedBytes<'fixture>, AbiResult> {
        let guard = lock()?;
        self.owned_bytes_locked(input, &guard)
    }

    fn owned_bytes_locked<'fixture>(
        &'fixture mut self,
        input: &[u8],
        _guard: &MutexGuard<'static, ()>,
    ) -> Result<OwnedBytes<'fixture>, AbiResult> {
        let input_len = length(input.len())?;
        let mut bytes = std::ptr::null_mut();
        let mut out_len = 0_u32;
        unsafe {
            result(ra8_abi_fixture_bytes_create(
                self.raw.ok_or(AbiResult::State)?.as_ptr(),
                input.as_ptr(),
                input_len,
                &raw mut bytes,
                &raw mut out_len,
            ))?;
        }
        let bytes = NonNull::new(bytes).ok_or(AbiResult::Null)?;
        Ok(OwnedBytes {
            raw: Some(bytes),
            len: out_len as usize,
            fixture: PhantomData,
        })
    }

    /// Exercise owned-byte allocation failure under this serialized session.
    ///
    /// # Errors
    ///
    /// Returns [`AbiResult::NoMemory`] when the provider honors its test
    /// control. Any other provider result is returned unchanged.
    pub fn owned_bytes_with_forced_allocation_failure_for_test<'fixture>(
        &'fixture mut self,
        input: &[u8],
    ) -> Result<OwnedBytes<'fixture>, AbiResult> {
        let guard = lock()?;
        unsafe { ra8_abi_fixture_test_fail_next_allocation() };
        self.owned_bytes_locked(input, &guard)
    }

    /// Release the handle exactly once.
    ///
    /// # Errors
    ///
    /// Returns the provider error while retaining ownership for retry.
    pub fn close(&mut self) -> Result<(), AbiResult> {
        let mut raw = self.raw.ok_or(AbiResult::State)?.as_ptr();
        let _guard = lock()?;
        unsafe { result(ra8_abi_fixture_destroy(&raw mut raw))? };
        if !raw.is_null() {
            return Err(AbiResult::State);
        }
        self.raw = None;
        Ok(())
    }
}

impl Drop for Fixture {
    fn drop(&mut self) {
        if let Some(raw) = self.raw {
            let mut pointer = raw.as_ptr();
            let _guard = lock_for_drop();
            if unsafe { result(ra8_abi_fixture_destroy(&raw mut pointer)) }.is_ok() {
                self.raw = None;
            }
        }
    }
}

/// Library-owned bytes released automatically through the public C ABI.
pub struct OwnedBytes<'fixture> {
    raw: Option<NonNull<u8>>,
    len: usize,
    fixture: PhantomData<&'fixture mut Fixture>,
}

impl OwnedBytes<'_> {
    /// Borrow the provider-owned span for this guard's lifetime.
    #[must_use]
    pub fn as_slice(&self) -> &[u8] {
        let Some(raw) = self.raw else {
            return &[];
        };
        unsafe { std::slice::from_raw_parts(raw.as_ptr(), self.len) }
    }

    /// Release the bytes explicitly.
    ///
    /// # Errors
    ///
    /// Returns the provider error while retaining ownership for retry.
    pub fn release(&mut self) -> Result<(), AbiResult> {
        let mut raw = self.raw.ok_or(AbiResult::State)?.as_ptr();
        let _guard = lock()?;
        unsafe { result(ra8_abi_fixture_bytes_release(&raw mut raw))? };
        if !raw.is_null() {
            return Err(AbiResult::State);
        }
        self.raw = None;
        Ok(())
    }
}

impl Drop for OwnedBytes<'_> {
    fn drop(&mut self) {
        if let Some(raw) = self.raw {
            let mut pointer = raw.as_ptr();
            let _guard = lock_for_drop();
            if unsafe { result(ra8_abi_fixture_bytes_release(&raw mut pointer)) }.is_ok() {
                self.raw = None;
            }
        }
    }
}
