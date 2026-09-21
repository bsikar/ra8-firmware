// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie
//! Native Rust acceptance tests for the safe ABI boundary types.

use std::cell::Cell;

use ra8_zig_abi_fixture::{AbiResult, Backend, Client, validate_capacity};

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
