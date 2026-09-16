// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Audited Rust-to-C command-line policy membrane.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use crate::ReportSummary;
use std::ffi::{c_char, c_int, c_void, CString, OsString};
#[cfg(unix)]
use std::os::unix::ffi::OsStrExt as _;

unsafe extern "C" {
    fn priv_firmware_report_parse_args(
        argc: c_int,
        argv: *mut *mut c_char,
        out_path: *mut *const c_char,
    ) -> c_int;
    fn firmware_report_create(data: *const u8, size: usize, out_handle: *mut *mut c_void) -> c_int;
    fn firmware_report_query(handle: *mut c_void, out_summary: *mut ReportSummary) -> c_int;
    fn firmware_report_release(handle: *mut *mut c_void) -> c_int;
}

fn argument_bytes(argument: &OsString) -> Vec<u8> {
    #[cfg(unix)]
    {
        argument.as_os_str().as_bytes().to_vec()
    }
    #[cfg(not(unix))]
    {
        argument.to_string_lossy().as_bytes().to_vec()
    }
}

pub fn parse_path(arguments: &[OsString]) -> Result<OsString, ()> {
    let strings: Vec<CString> = arguments
        .iter()
        .map(|argument| CString::new(argument_bytes(argument)).map_err(|_| ()))
        .collect::<Result<_, _>>()?;
    let mut pointers: Vec<*mut c_char> = strings
        .iter()
        .map(|argument| argument.as_ptr().cast_mut())
        .collect();
    let argc = c_int::try_from(pointers.len()).map_err(|_| ())?;
    let mut path = std::ptr::null();
    // SAFETY: every pointer names a live terminated CString and path is writable.
    let status =
        unsafe { priv_firmware_report_parse_args(argc, pointers.as_mut_ptr(), &raw mut path) };
    if status != 0 || path.is_null() {
        return Err(());
    }
    Ok(arguments[1].clone())
}

pub fn summarize(image: &[u8]) -> Result<ReportSummary, ()> {
    crate::retain_foreign_exports();
    let data = if image.is_empty() {
        std::ptr::null()
    } else {
        image.as_ptr()
    };
    let mut handle = std::ptr::null_mut();
    // SAFETY: data is null for empty input or points to image.len() live bytes.
    if unsafe { firmware_report_create(data, image.len(), &raw mut handle) } != 0
        || handle.is_null()
    {
        return Err(());
    }
    let mut summary = ReportSummary {
        byte_count: 0,
        zero_count: 0,
        erased_count: 0,
        fnv1a64: 0,
    };
    // SAFETY: handle is owned and live, and summary is writable.
    let query_status = unsafe { firmware_report_query(handle, &raw mut summary) };
    // SAFETY: handle is uniquely owned and this is its only release.
    let release_status = unsafe { firmware_report_release(&raw mut handle) };
    if query_status != 0 || release_status != 0 || !handle.is_null() {
        return Err(());
    }
    Ok(summary)
}
