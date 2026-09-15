// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Audited Rust-to-Zig C ABI adapter.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use std::ptr;

use crate::{RustSummary, analyze};

const MAX_IMAGE_SIZE: usize = 16 * 1024 * 1024;

#[derive(Clone, Copy)]
#[repr(i32)]
pub enum PipelineStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidSize = 2,
    EmptyImage = 3,
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn firmware_pipeline_rust_analyze(
    data: *const u8,
    size: usize,
    out_summary: *mut RustSummary,
) -> PipelineStatus {
    if data.is_null() || out_summary.is_null() {
        return PipelineStatus::InvalidArgument;
    }
    if size > MAX_IMAGE_SIZE {
        return PipelineStatus::InvalidSize;
    }
    // SAFETY: the private ABI requires data to name size readable bytes.
    let image = unsafe { std::slice::from_raw_parts(data, size) };
    let Some(summary) = analyze(image) else {
        return PipelineStatus::EmptyImage;
    };
    // SAFETY: output was validated and is written only after all failure checks.
    unsafe { ptr::write(out_summary, summary) };
    PipelineStatus::Ok
}

const _: unsafe extern "C" fn(*const u8, usize, *mut RustSummary) -> PipelineStatus =
    firmware_pipeline_rust_analyze;
const _: () = assert!(size_of::<PipelineStatus>() == 4);
const _: () = assert!(size_of::<RustSummary>() == 32);
const _: () = assert!(align_of::<RustSummary>() == 8);
const _: () = assert!(std::mem::offset_of!(RustSummary, fnv1a64) == 24);
