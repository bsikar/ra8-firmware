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

#[cfg(test)]
mod tests {
    use std::cell::Cell;

    use super::{AbiResult, Backend, Client, validate_capacity};

    struct MockBackend {
        create_result: Result<u32, AbiResult>,
        copy_result: Result<usize, AbiResult>,
        destroys: Cell<usize>,
        fail_destroy_once: Cell<bool>,
    }

    impl Backend for MockBackend {
        type Handle = u32;

        fn create(&self) -> Result<Self::Handle, AbiResult> {
            self.create_result
        }

        fn copy(&self, _handle: &mut Self::Handle, output: &mut [u8]) -> Result<usize, AbiResult> {
            let count = self.copy_result?;
            if output.len() < count {
                return Err(AbiResult::Length);
            }
            output[..count].fill(b'x');
            Ok(count)
        }

        fn destroy(&self, handle: &mut Option<Self::Handle>) -> Result<(), AbiResult> {
            if self.fail_destroy_once.replace(false) {
                return Err(AbiResult::State);
            }
            handle.take().ok_or(AbiResult::State)?;
            self.destroys.set(self.destroys.get() + 1);
            Ok(())
        }
    }

    fn backend(copy_result: Result<usize, AbiResult>) -> MockBackend {
        MockBackend {
            create_result: Ok(7),
            copy_result,
            destroys: Cell::new(0),
            fail_destroy_once: Cell::new(false),
        }
    }

    #[test]
    fn result_discriminants_match_the_c_header() {
        assert_eq!(AbiResult::Ok as u16, 0);
        assert_eq!(AbiResult::NoMemory as u16, 0x102);
        assert_eq!(AbiResult::InvalidArgument as u16, 0x103);
        assert_eq!(AbiResult::State as u16, 0x104);
        assert_eq!(AbiResult::Length as u16, 0x105);
        assert_eq!(AbiResult::Busy as u16, 0x109);
        assert_eq!(AbiResult::Null as u16, 0x504);
    }

    #[test]
    fn every_defined_result_converts() {
        for raw in [0, 0x102, 0x103, 0x104, 0x105, 0x109, 0x504] {
            assert!(AbiResult::try_from(raw).is_ok());
        }
    }

    #[test]
    fn unknown_result_is_preserved() {
        assert_eq!(AbiResult::try_from(u16::MAX), Err(u16::MAX));
    }

    #[test]
    fn zero_length_accepts_zero_capacity() {
        assert_eq!(validate_capacity(0, 0), AbiResult::Ok);
    }

    #[test]
    fn exact_capacity_is_accepted() {
        assert_eq!(validate_capacity(8, 8), AbiResult::Ok);
    }

    #[test]
    fn excess_capacity_is_accepted() {
        assert_eq!(validate_capacity(8, 9), AbiResult::Ok);
    }

    #[test]
    fn short_capacity_is_rejected() {
        assert_eq!(validate_capacity(8, 7), AbiResult::Length);
    }

    #[test]
    fn extreme_length_does_not_wrap() {
        assert_eq!(validate_capacity(usize::MAX, usize::MAX), AbiResult::Ok);
    }

    #[test]
    fn acquisition_error_is_preserved_without_cleanup() {
        let backend = MockBackend {
            create_result: Err(AbiResult::NoMemory),
            copy_result: Ok(0),
            destroys: Cell::new(0),
            fail_destroy_once: Cell::new(false),
        };
        assert!(matches!(Client::new(&backend), Err(AbiResult::NoMemory)));
        assert_eq!(backend.destroys.get(), 0);
    }

    #[test]
    fn null_output_is_rejected_before_the_backend_call() {
        let backend = backend(Ok(1));
        let mut client = Client::new(&backend).expect("fixture acquisition");
        assert_eq!(client.copy_into(None), Err(AbiResult::Null));
    }

    #[test]
    fn short_output_preserves_length_error() {
        let backend = backend(Ok(2));
        let mut client = Client::new(&backend).expect("fixture acquisition");
        assert_eq!(client.copy_into(Some(&mut [0; 1])), Err(AbiResult::Length));
    }

    #[test]
    fn successful_copy_reports_length_and_bytes() {
        let backend = backend(Ok(2));
        let mut client = Client::new(&backend).expect("fixture acquisition");
        let mut output = [0; 2];
        assert_eq!(client.copy_into(Some(&mut output)), Ok(2));
        assert_eq!(output, *b"xx");
    }

    #[test]
    fn explicit_close_transfers_ownership_once() {
        let backend = backend(Ok(0));
        let mut client = Client::new(&backend).expect("fixture acquisition");
        client.close().expect("fixture cleanup");
        assert_eq!(backend.destroys.get(), 1);
        assert_eq!(client.close(), Err(AbiResult::State));
        assert_eq!(backend.destroys.get(), 1);
    }

    #[test]
    fn failed_close_retains_ownership_for_retry() {
        let backend = MockBackend {
            create_result: Ok(7),
            copy_result: Ok(0),
            destroys: Cell::new(0),
            fail_destroy_once: Cell::new(true),
        };
        let mut client = Client::new(&backend).expect("fixture acquisition");
        assert_eq!(client.close(), Err(AbiResult::State));
        assert_eq!(backend.destroys.get(), 0);
        client.close().expect("retry cleanup");
        assert_eq!(backend.destroys.get(), 1);
    }

    #[test]
    fn drop_cleans_up_an_owned_handle_once() {
        let backend = backend(Ok(0));
        {
            let _client = Client::new(&backend).expect("fixture acquisition");
        }
        assert_eq!(backend.destroys.get(), 1);
    }
}
