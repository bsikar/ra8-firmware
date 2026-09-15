// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Safe, native-Rust representations of the ABI fixture's boundary values.
//!
//! Cross-language linkage is deliberately added by the directional ABI
//! issues. These types let Rust's own test runner lock down the value and
//! buffer contracts before an `unsafe extern "C"` adapter is introduced.

/// Stable result values exported by the ABI fixture.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
#[repr(u16)]
pub enum AbiResult {
    /// The operation completed successfully.
    Ok = 0,
    /// A required pointer was null.
    Null = 0x504,
    /// A supplied length or capacity was invalid.
    Length = 0x105,
    /// Allocation failed without transferring ownership.
    NoMemory = 0x102,
    /// An argument was invalid.
    InvalidArgument = 0x103,
    /// The opaque handle or its state was invalid.
    State = 0x104,
    /// The resource is currently busy.
    Busy = 0x109,
}

impl TryFrom<u16> for AbiResult {
    type Error = u16;

    fn try_from(value: u16) -> Result<Self, Self::Error> {
        match value {
            0 => Ok(Self::Ok),
            0x102 => Ok(Self::NoMemory),
            0x103 => Ok(Self::InvalidArgument),
            0x104 => Ok(Self::State),
            0x105 => Ok(Self::Length),
            0x109 => Ok(Self::Busy),
            0x504 => Ok(Self::Null),
            unknown => Err(unknown),
        }
    }
}

/// Validate the pointer-independent part of a caller-owned output contract.
#[must_use]
pub const fn validate_capacity(required: usize, capacity: usize) -> AbiResult {
    if capacity < required {
        AbiResult::Length
    } else {
        AbiResult::Ok
    }
}

/// Safe behavior required from a future `extern "C"` backend adapter.
pub trait Backend {
    /// Opaque ownership token returned by the foreign provider.
    type Handle;

    /// Acquire one foreign handle.
    ///
    /// # Errors
    ///
    /// Returns the provider's stable ABI error without acquiring ownership.
    fn create(&self) -> Result<Self::Handle, AbiResult>;

    /// Copy bytes into caller-owned storage.
    ///
    /// # Errors
    ///
    /// Returns the provider's stable ABI error without transferring the buffer.
    fn copy(&self, handle: &mut Self::Handle, output: &mut [u8]) -> Result<usize, AbiResult>;

    /// Release one foreign handle.
    /// On success, implementations must take the value and leave `handle` as
    /// `None` exactly once. On failure, they must leave the identical
    /// `Some(handle)` value intact so the caller can retry cleanup.
    ///
    /// # Errors
    ///
    /// Returns the provider's stable ABI error if cleanup cannot complete.
    fn destroy(&self, handle: &mut Option<Self::Handle>) -> Result<(), AbiResult>;
}

/// Rust-owned lifetime guard around an ABI handle.
pub struct Client<'backend, B: Backend> {
    backend: &'backend B,
    handle: Option<B::Handle>,
}

impl<'backend, B: Backend> Client<'backend, B> {
    /// Acquire a handle without exposing it to safe callers.
    ///
    /// # Errors
    ///
    /// Returns the backend's stable error when acquisition fails.
    pub fn new(backend: &'backend B) -> Result<Self, AbiResult> {
        Ok(Self {
            backend,
            handle: Some(backend.create()?),
        })
    }

    /// Validate a nullable caller buffer before crossing the ABI boundary.
    ///
    /// # Errors
    ///
    /// Returns [`AbiResult::Null`], [`AbiResult::State`], or a backend error.
    pub fn copy_into(&mut self, output: Option<&mut [u8]>) -> Result<usize, AbiResult> {
        let output = output.ok_or(AbiResult::Null)?;
        let handle = self.handle.as_mut().ok_or(AbiResult::State)?;
        self.backend.copy(handle, output)
    }

    /// Release the handle exactly once and report provider failures.
    ///
    /// # Errors
    ///
    /// Returns [`AbiResult::State`] or the backend's cleanup error.
    pub fn close(&mut self) -> Result<(), AbiResult> {
        if self.handle.is_none() {
            return Err(AbiResult::State);
        }
        self.backend.destroy(&mut self.handle)
    }
}

impl<B: Backend> Drop for Client<'_, B> {
    fn drop(&mut self) {
        if self.handle.is_some() {
            let _ = self.backend.destroy(&mut self.handle);
        }
    }
}
