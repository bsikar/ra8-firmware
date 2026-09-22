// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Narrow unsafe adapter for the hand-authored C23 interface.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use std::ptr;
use std::sync::Mutex;

use crate::{ReportSummary, summarize};

const MAX_IMAGE_SIZE: usize = 16 * 1024 * 1024;

struct Registry {
    generation: usize,
    live: Option<ReportSummary>,
}

static LIVE_HANDLE: Mutex<Registry> = Mutex::new(Registry {
    generation: 0,
    live: None,
});

#[derive(Clone, Copy)]
#[repr(i32)]
pub enum ReportStatus {
    Ok = 0,
    InvalidArgument = 1,
    InvalidSize = 2,
    CapacityUnavailable = 3,
    Internal = 4,
}

pub enum ReportHandle {}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn firmware_report_create(
    data: *const u8,
    size: usize,
    out_handle: *mut *mut ReportHandle,
) -> ReportStatus {
    if out_handle.is_null() || (data.is_null() && size != 0) {
        return ReportStatus::InvalidArgument;
    }
    if size > MAX_IMAGE_SIZE {
        return ReportStatus::InvalidSize;
    }
    // SAFETY: the public ABI requires a writable pointer slot.
    if unsafe { !(*out_handle).is_null() } {
        return ReportStatus::InvalidArgument;
    }
    let Ok(mut live) = LIVE_HANDLE.lock() else {
        return ReportStatus::Internal;
    };
    if live.live.is_some() {
        return ReportStatus::CapacityUnavailable;
    }
    let image = if size == 0 {
        &[]
    } else {
        // SAFETY: the ABI requires `data` to name `size` readable bytes.
        unsafe { std::slice::from_raw_parts(data, size) }
    };
    let Some(generation) = live.generation.checked_add(1) else {
        return ReportStatus::Internal;
    };
    live.generation = generation;
    live.live = Some(summarize(image));
    let handle = ptr::with_exposed_provenance_mut(generation);
    // SAFETY: validated writable slot; the non-repeating opaque token is borrowed by C.
    unsafe { *out_handle = handle };
    ReportStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn firmware_report_query(
    handle: *const ReportHandle,
    out_summary: *mut ReportSummary,
) -> ReportStatus {
    if handle.is_null() || out_summary.is_null() {
        return ReportStatus::InvalidArgument;
    }
    let Ok(live) = LIVE_HANDLE.lock() else {
        return ReportStatus::Internal;
    };
    let Some(summary) = live.live else {
        return ReportStatus::InvalidArgument;
    };
    if handle.addr() != live.generation {
        return ReportStatus::InvalidArgument;
    }
    // SAFETY: validated output pointer; write occurs only after every failure check.
    unsafe { ptr::write(out_summary, summary) };
    ReportStatus::Ok
}

#[unsafe(no_mangle)]
pub unsafe extern "C" fn firmware_report_release(
    in_out_handle: *mut *mut ReportHandle,
) -> ReportStatus {
    if in_out_handle.is_null() {
        return ReportStatus::InvalidArgument;
    }
    // SAFETY: the public ABI requires a readable and writable pointer slot.
    let handle = unsafe { *in_out_handle };
    if handle.is_null() {
        return ReportStatus::InvalidArgument;
    }
    let Ok(mut live) = LIVE_HANDLE.lock() else {
        return ReportStatus::Internal;
    };
    if live.live.is_none() {
        return ReportStatus::InvalidArgument;
    }
    if handle.addr() != live.generation {
        return ReportStatus::InvalidArgument;
    }
    live.live = None;
    // SAFETY: validated writable slot; clearing ends the caller's token borrow.
    unsafe { *in_out_handle = ptr::null_mut() };
    ReportStatus::Ok
}

const _: unsafe extern "C" fn(*const u8, usize, *mut *mut ReportHandle) -> ReportStatus =
    firmware_report_create;
const _: unsafe extern "C" fn(*const ReportHandle, *mut ReportSummary) -> ReportStatus =
    firmware_report_query;
const _: unsafe extern "C" fn(*mut *mut ReportHandle) -> ReportStatus = firmware_report_release;
const _: () = assert!(size_of::<ReportStatus>() == 4);
const _: () = assert!(size_of::<ReportSummary>() == 32);
const _: () = assert!(align_of::<ReportSummary>() == 8);
const _: () = assert!(std::mem::offset_of!(ReportSummary, byte_count) == 0);
const _: () = assert!(std::mem::offset_of!(ReportSummary, zero_count) == 8);
const _: () = assert!(std::mem::offset_of!(ReportSummary, erased_count) == 16);
const _: () = assert!(std::mem::offset_of!(ReportSummary, fnv1a64) == 24);
