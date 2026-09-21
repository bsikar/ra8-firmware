// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

#![expect(
    unsafe_code,
    reason = "these tests directly exercise the audited C ABI"
)]

use std::ffi::c_void;
use std::sync::Mutex;

use firmware_report_provider::ReportSummary;

static PROVIDER_TEST_LOCK: Mutex<()> = Mutex::new(());

unsafe extern "C" {
    fn firmware_report_create(data: *const u8, size: usize, out: *mut *mut c_void) -> i32;
    fn firmware_report_query(handle: *const c_void, out: *mut ReportSummary) -> i32;
    fn firmware_report_release(handle: *mut *mut c_void) -> i32;
}

#[test]
fn foreign_lifecycle_and_unchanged_outputs() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let input = [0, 0xff, 7];
    let mut handle = std::ptr::null_mut();
    // SAFETY: all pointers and lengths satisfy the declared test ABI.
    assert_eq!(
        unsafe { firmware_report_create(input.as_ptr(), input.len(), &raw mut handle) },
        0
    );
    assert!(!handle.is_null());
    let sentinel = ReportSummary {
        byte_count: u64::MAX,
        zero_count: u64::MAX,
        erased_count: u64::MAX,
        fnv1a64: u64::MAX,
    };
    let mut output = sentinel;
    // SAFETY: handle is live and output is writable.
    assert_eq!(unsafe { firmware_report_query(handle, &raw mut output) }, 0);
    assert_eq!(output.byte_count, 3);
    let saved = output;
    // SAFETY: null is an invalid handle and must be rejected before output access.
    assert_eq!(
        unsafe { firmware_report_query(std::ptr::null(), &raw mut output) },
        1
    );
    assert_eq!(output, saved);
    let invalid = std::ptr::with_exposed_provenance(usize::MAX);
    // SAFETY: the foreign non-null address is compared with the registry and never dereferenced.
    assert_eq!(
        unsafe { firmware_report_query(invalid, &raw mut output) },
        1
    );
    assert_eq!(output, saved);
    // SAFETY: handle slot owns the unique live provider borrow.
    assert_eq!(unsafe { firmware_report_release(&raw mut handle) }, 0);
    assert!(handle.is_null());
    // SAFETY: cleared slot exercises repeated teardown rejection.
    assert_eq!(unsafe { firmware_report_release(&raw mut handle) }, 1);
}

#[test]
fn foreign_create_rejects_invalid_inputs_without_output() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let mut handle = std::ptr::null_mut();
    // SAFETY: deliberately invalid pointer/length pair is rejected before dereference.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 1, &raw mut handle) },
        1
    );
    assert!(handle.is_null());
    // SAFETY: size is rejected before the non-null placeholder is dereferenced.
    assert_eq!(
        unsafe {
            firmware_report_create(std::ptr::dangling(), 16 * 1024 * 1024 + 1, &raw mut handle)
        },
        2
    );
    assert!(handle.is_null());
}

#[test]
fn foreign_create_enforces_single_live_owner() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let mut first = std::ptr::null_mut();
    let mut second = std::ptr::null_mut();
    // SAFETY: empty images permit null data and valid output slots.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut first) },
        0
    );
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut second) },
        3
    );
    assert!(second.is_null());
    // SAFETY: first is the unique live provider borrow.
    assert_eq!(unsafe { firmware_report_release(&raw mut first) }, 0);
}

#[test]
fn foreign_query_rejects_null_output() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let mut handle = std::ptr::null_mut();
    // SAFETY: valid empty input and output slot.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut handle) },
        0
    );
    // SAFETY: null output is rejected before a write.
    assert_eq!(
        unsafe { firmware_report_query(handle, std::ptr::null_mut()) },
        1
    );
    // SAFETY: handle remains live after the failed query.
    assert_eq!(unsafe { firmware_report_release(&raw mut handle) }, 0);
}

#[test]
fn foreign_create_rejects_occupied_output_slot() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let mut handle = std::ptr::dangling_mut::<c_void>();
    // SAFETY: occupied output is rejected before the placeholder is dereferenced.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut handle) },
        1
    );
    assert_eq!(handle, std::ptr::dangling_mut());
}

#[test]
fn foreign_release_rejects_null_slot() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    // SAFETY: null slot is deliberately passed to exercise validation.
    assert_eq!(unsafe { firmware_report_release(std::ptr::null_mut()) }, 1);
}

#[test]
fn foreign_stale_alias_cannot_access_a_later_generation() {
    let _guard = PROVIDER_TEST_LOCK.lock().expect("provider test lock");
    let mut first = std::ptr::null_mut();
    // SAFETY: valid empty input and writable handle slot.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut first) },
        0
    );
    let mut stale = first;
    // SAFETY: first contains the unique live token.
    assert_eq!(unsafe { firmware_report_release(&raw mut first) }, 0);

    let mut second = std::ptr::null_mut();
    // SAFETY: capacity is available for a new generation.
    assert_eq!(
        unsafe { firmware_report_create(std::ptr::null(), 0, &raw mut second) },
        0
    );
    let mut output = ReportSummary {
        byte_count: u64::MAX,
        zero_count: u64::MAX,
        erased_count: u64::MAX,
        fnv1a64: u64::MAX,
    };
    let saved = output;
    // SAFETY: stale is non-null but must fail token comparison before any access.
    assert_eq!(unsafe { firmware_report_query(stale, &raw mut output) }, 1);
    assert_eq!(output, saved);
    assert_eq!(unsafe { firmware_report_release(&raw mut stale) }, 1);
    assert!(!stale.is_null());
    // SAFETY: the failed stale release leaves the current generation live.
    assert_eq!(unsafe { firmware_report_release(&raw mut second) }, 0);
}
