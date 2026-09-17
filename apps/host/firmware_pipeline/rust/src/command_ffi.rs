// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Audited Rust adapters for the C support and Zig public ABI.

#![expect(unsafe_code, reason = "this module is the audited C ABI adapter")]

use std::ffi::{CString, OsString, c_char, c_int, c_void};
use std::os::unix::ffi::OsStrExt as _;

const ABI_VERSION: u32 = 0x5a49_5001;

#[repr(C)]
pub struct Image {
    bytes: *mut u8,
    size: usize,
}

#[repr(C)]
struct Config {
    abi_version: u32,
    reserved0: u32,
}

#[derive(Default)]
#[repr(C)]
struct ResultRecord {
    byte_count: u64,
    zero_count: u64,
    erased_count: u64,
    fnv1a64: u64,
    zig_xor8: u8,
    zig_stage_marker: u8,
    reserved: [u8; 6],
}

unsafe extern "C" {
    fn priv_firmware_pipeline_parse_args(
        argc: c_int,
        argv: *mut *mut c_char,
        out_path: *mut *const c_char,
    ) -> c_int;
    fn priv_firmware_pipeline_host_io() -> *const c_void;
    fn priv_firmware_pipeline_read_image(
        ops: *const c_void,
        path: *const c_char,
        out_image: *mut Image,
    ) -> u8;
    fn priv_firmware_pipeline_release_image(ops: *const c_void, image: *mut Image);
    fn firmware_pipeline_analyze(
        config: *const Config,
        data: *const u8,
        size: usize,
        out_result: *mut ResultRecord,
    ) -> i32;
}

pub struct OwnedImage {
    ops: *const c_void,
    image: Image,
}

impl OwnedImage {
    pub fn read(arguments: &[OsString]) -> Result<Self, &'static str> {
        let strings: Vec<CString> = arguments
            .iter()
            .map(|argument| CString::new(argument.as_os_str().as_bytes()).map_err(|_| "usage"))
            .collect::<Result<_, _>>()?;
        let mut pointers: Vec<*mut c_char> = strings
            .iter()
            .map(|argument| argument.as_ptr().cast_mut())
            .collect();
        let argc = c_int::try_from(pointers.len()).map_err(|_| "usage")?;
        let mut path = std::ptr::null();
        // SAFETY: arguments are live terminated strings and path is writable.
        if unsafe { priv_firmware_pipeline_parse_args(argc, pointers.as_mut_ptr(), &raw mut path) }
            != 0
        {
            return Err("usage");
        }
        if path.is_null() {
            return Err("usage");
        }
        // SAFETY: the C runtime returns an immutable process-lifetime table.
        let ops = unsafe { priv_firmware_pipeline_host_io() };
        let mut image = Image {
            bytes: std::ptr::null_mut(),
            size: 0,
        };
        // SAFETY: ops/path are valid and image is writable and initially empty.
        if unsafe { priv_firmware_pipeline_read_image(ops, path, &raw mut image) } != 0 {
            return Err("cannot read bounded input");
        }
        Ok(Self { ops, image })
    }

    pub fn analyze(&self) -> Result<String, &'static str> {
        let placeholder = 0_u8;
        let data = if self.image.bytes.is_null() {
            &placeholder
        } else {
            // SAFETY: the owned C image remains live for this borrow.
            unsafe { &*self.image.bytes }
        };
        let config = Config {
            abi_version: ABI_VERSION,
            reserved0: 0,
        };
        let mut result = ResultRecord::default();
        // SAFETY: inputs meet the public ABI and output is writable.
        let status = unsafe {
            firmware_pipeline_analyze(&raw const config, data, self.image.size, &raw mut result)
        };
        if status != 0 {
            return Err(if status == 3 {
                "Rust rejected empty image"
            } else {
                "language pipeline failed"
            });
        }
        Ok(format!(
            "bytes={}\nzero={}\nerased={}\nfnv1a64={:016x}\nzig_xor8={:02x}\nzig_stage={:02x}\n",
            result.byte_count,
            result.zero_count,
            result.erased_count,
            result.fnv1a64,
            result.zig_xor8,
            result.zig_stage_marker
        ))
    }
}

impl Drop for OwnedImage {
    fn drop(&mut self) {
        // SAFETY: this object uniquely owns the image and releases it once.
        unsafe { priv_firmware_pipeline_release_image(self.ops, &raw mut self.image) };
    }
}
