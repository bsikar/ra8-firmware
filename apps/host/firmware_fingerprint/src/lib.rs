// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Brighton Sikarskie

//! Deterministic firmware-image fingerprints and the CLI's testable core.

use std::ffi::OsString;
use std::io::{self, Write};
use std::path::{Path, PathBuf};

/// The FNV-1a 64-bit offset basis.
const FNV_OFFSET_BASIS: u64 = 0xcbf2_9ce4_8422_2325;
/// The FNV-1a 64-bit prime.
const FNV_PRIME: u64 = 0x0000_0100_0000_01b3;

/// Stable summary of one firmware image.
#[derive(Clone, Copy, Debug, Eq, PartialEq)]
pub struct Fingerprint {
    /// Number of bytes in the image.
    pub bytes: usize,
    /// FNV-1a 64-bit digest of the complete image.
    pub fnv1a64: u64,
}

/// Compute a deterministic, dependency-free fingerprint over `image`.
#[must_use]
pub fn fingerprint(image: &[u8]) -> Fingerprint {
    let mut digest = FNV_OFFSET_BASIS;
    for byte in image {
        digest ^= u64::from(*byte);
        digest = digest.wrapping_mul(FNV_PRIME);
    }
    Fingerprint {
        bytes: image.len(),
        fnv1a64: digest,
    }
}

/// Execute the CLI with injectable file and output operations.
///
/// Returns the process exit code: zero on success and two for usage or I/O
/// failures. Diagnostics are written only to `stderr`.
pub fn run<I, ReadFile, Stdout, Stderr>(
    args: I,
    read_file: ReadFile,
    stdout: &mut Stdout,
    stderr: &mut Stderr,
) -> i32
where
    I: IntoIterator<Item = OsString>,
    ReadFile: FnOnce(&Path) -> io::Result<Vec<u8>>,
    Stdout: Write,
    Stderr: Write,
{
    let mut args = args.into_iter();
    let Some(path) = args.next().map(PathBuf::from) else {
        let _ = writeln!(stderr, "usage: firmware_fingerprint <firmware-image>");
        return 2;
    };
    if args.next().is_some() {
        let _ = writeln!(stderr, "usage: firmware_fingerprint <firmware-image>");
        return 2;
    }

    let image = match read_file(&path) {
        Ok(image) => image,
        Err(error) => {
            let _ = writeln!(stderr, "firmware_fingerprint: {}: {error}", path.display());
            return 2;
        }
    };
    let result = fingerprint(&image);
    if writeln!(stdout, "bytes={}", result.bytes).is_err()
        || writeln!(stdout, "fnv1a64={:016x}", result.fnv1a64).is_err()
    {
        let _ = writeln!(stderr, "firmware_fingerprint: failed to write output");
        return 2;
    }
    0
}
