// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

#![expect(unsafe_code, reason = "tests directly exercise the audited C ABI")]

use firmware_pipeline_rust::RustSummary;

unsafe extern "C" {
    fn firmware_pipeline_rust_analyze(data: *const u8, size: usize, out: *mut RustSummary) -> i32;
}

fn sentinel() -> RustSummary {
    RustSummary {
        byte_count: u64::MAX,
        zero_count: u64::MAX,
        erased_count: u64::MAX,
        fnv1a64: u64::MAX,
    }
}

#[test]
fn foreign_success() {
    let input = [0, 0xff, 7];
    let mut output = sentinel();
    // SAFETY: input and output satisfy the declared private ABI.
    assert_eq!(
        unsafe { firmware_pipeline_rust_analyze(input.as_ptr(), input.len(), &raw mut output) },
        0
    );
    assert_eq!(output.byte_count, 3);
}

#[test]
fn foreign_empty_preserves_output() {
    let mut output = sentinel();
    let saved = output;
    // SAFETY: non-null placeholder is never dereferenced for an empty input.
    assert_eq!(
        unsafe { firmware_pipeline_rust_analyze(std::ptr::dangling(), 0, &raw mut output) },
        3
    );
    assert_eq!(output, saved);
}

#[test]
fn foreign_null_preserves_output() {
    let mut output = sentinel();
    let saved = output;
    // SAFETY: null input is rejected before dereference.
    assert_eq!(
        unsafe { firmware_pipeline_rust_analyze(std::ptr::null(), 1, &raw mut output) },
        1
    );
    assert_eq!(output, saved);
}

#[test]
fn foreign_null_output_is_rejected() {
    let input = [1];
    // SAFETY: the null output is rejected before it can be written.
    assert_eq!(
        unsafe {
            firmware_pipeline_rust_analyze(input.as_ptr(), input.len(), std::ptr::null_mut())
        },
        1
    );
}

#[test]
fn foreign_oversize_preserves_output() {
    let mut output = sentinel();
    let saved = output;
    // SAFETY: the size limit is rejected before the one-byte placeholder is read.
    assert_eq!(
        unsafe {
            firmware_pipeline_rust_analyze(
                std::ptr::dangling(),
                16 * 1024 * 1024 + 1,
                &raw mut output,
            )
        },
        2
    );
    assert_eq!(output, saved);
}
